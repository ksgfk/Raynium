#include <raynium/renderer.h>
#include <raynium/surface_submission.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <algorithm>
#include <span>
#include <chrono>
#include <utility>

namespace raynium {
bool Renderer::Create(radray::DeviceContext& device,std::span<const radray::byte> shaders,uint32_t flags,radray::unique_ptr<Renderer>& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Create");
    if(out) RADRAY_ABORT("Renderer output occupied");
    using namespace radray::gpu;
    const auto& caps=device.GetDevice().GetCapabilities();
    bool indirect=false;
    if(!SelectTerrainSubmission(flags,caps,indirect)) return false;
    const radray::array layouts{VertexBufferLayout{0,28,VertexStepMode::Vertex},VertexBufferLayout{1,16,VertexStepMode::Instance}};
    const radray::array attributes{radray::VertexInputAttribute{0,0,"POSITION",0,VertexFormat::FLOAT32X3},
        radray::VertexInputAttribute{0,12,"COLOR",0,VertexFormat::UNORM8X4},
        radray::VertexInputAttribute{0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2},
        radray::VertexInputAttribute{0,24,"TEXCOORD",1,VertexFormat::UINT16X2},
        radray::VertexInputAttribute{1,0,"TEXCOORD",2,VertexFormat::SINT32X3},
        radray::VertexInputAttribute{1,12,"TEXCOORD",3,VertexFormat::FLOAT32}};
    constexpr uint64_t targetPageBytes=512ull<<20;
    const auto pageBytes=caps.Limits.MaxBufferSize?std::min(targetPageBytes,caps.Limits.MaxBufferSize):targetPageBytes;
    auto vertices=radray::VertexBufferPool::Create(device.GetDevice(),{.VertexStride=28,.PageVertexCapacity=uint32_t(pageBytes/28),
        .AdditionalUsage=BufferUse::Resource,.Layouts=layouts,.Attributes=attributes,.DebugNamePrefix="TerrainVertices"}).Release();
    auto translucentVertices=radray::VertexBufferPool::Create(device.GetDevice(),{.VertexStride=28,.PageVertexCapacity=uint32_t(pageBytes/28),
        .AdditionalUsage=BufferUse::Resource,.Layouts=layouts,.Attributes=attributes,.DebugNamePrefix="TranslucentVertices"}).Release();
    auto quadPool=radray::IndexBufferPool::Create(device.GetDevice(),{.PageIndexCapacity=(16u<<20)/sizeof(uint32_t),
        .AdditionalUsage=BufferUse::Resource,.DebugNamePrefix="QuadIndices"}).Release();
    auto indices=radray::IndexBufferPool::Create(device.GetDevice(),{.PageIndexCapacity=(128u<<20)/sizeof(uint32_t),
        .AdditionalUsage=BufferUse::Resource,.DebugNamePrefix="TranslucentIndices"}).Release();
    if(!vertices || !translucentVertices || !quadPool || !indices) return false;
    radray::unique_ptr<StaticGeometryRegistry> staticGeometry;
    if(!StaticGeometryRegistry::Create(device.GetDevice(),staticGeometry)) return false;
    radray::unique_ptr<ModularFramePipeline> pipeline;
    if(!ModularFramePipeline::Create(device,shaders,indirect,pipeline)) return false;
    auto* borrowed=pipeline.get();
    auto scheduler=radray::FrameScheduler::Create(device,std::move(pipeline),2).Release();
    if(!scheduler) return false;
    auto renderer=radray::unique_ptr<Renderer>(new Renderer(device,std::move(scheduler),std::move(vertices),
        std::move(translucentVertices),std::move(quadPool),std::move(indices),std::move(staticGeometry),borrowed));
    if(!renderer->_buffers.Prewarm(device.GetDevice())) return false;
    out=std::move(renderer);
    return true;
}
Renderer::~Renderer() noexcept try {
    _scheduler->WaitIdle();
    _staticGeometry->Reset();
    _terrain.Reset();
} catch(...) {
    AbortUnexpectedException("Renderer::~Renderer");
}
void Renderer::Finish() {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Finish");
    _pipeline->Finish(_device.GetDevice());
    _scheduler->WaitIdle();
}
bool Renderer::SubmitFrame(rn_renderer* owner,BatchEnvelope& envelope,FrameExecution& result,const std::function<void(uint64_t)>& notifySubmitted,const std::function<bool()>& collect) {
    using namespace radray::gpu;
    auto& device=_device.GetDevice();
    const auto fail=[&] {
        _poisoned=true;
        if(device.IsDeviceLost()) result.flags|=RN_SUBMIT_FLAG_DEVICE_LOST;
        return false;
    };
    const auto& batch=envelope.batch;
    bool submittedCaptures=false;
    auto cancelCaptures=radray::MakeScopeGuard([&]() noexcept {
        if(!submittedCaptures && batch.capture_count)
            _pipeline->CancelUnsubmittedCaptures({batch.captures,batch.capture_count});
    });
    if(_poisoned || device.IsDeviceLost()) return fail();
    auto* target=envelope.target;
    const bool measure=batch.flags&RN_BATCH_MEASURE;
    const auto now=[measure]()->uint64_t {
        return measure?uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()):0;
    };
    const auto start=now();
    auto& slot=_scheduler->BeginFrame();
    const auto waited=now();
    if(device.IsDeviceLost()) return fail();
    if (!collect()) return fail();
    _pipeline->MeasureBatch(slot,measure);
    auto& tape=*slot.Emplace<TapeCompiler>(slot);
    for(auto id:envelope.resources.removedModelMeshes) RemoveModelMesh(id);
    _staticGeometry->BeginSlot();
    _terrain.BeginSlotViews({batch.views,batch.view_count},slot);
    if(!_staticGeometry->PrepareQuadIndices(_terrain,slot,tape.WorldUploads()) ||
       !tape.PrepareViews(batch,_terrain,owner,_pipeline->Resources(),_pipeline->UsesIndirect()) ||
       !_pipeline->CompileTape(slot,tape,batch,owner,*_staticGeometry,_terrain)) {
        RADRAY_ERR_LOG("Invalid command tape or view");
        return fail();
    }
    _staticGeometry->FlushPending(slot,tape.WorldUploads());
    tape.FinishUploads();
    TerrainFrame frame{};
    frame.uploads=&envelope.resources;
    frame.shadowWrites=envelope.shadows;
    SwapChainAcquireResult acquired{.Status=SwapChainStatus::RetryLater};
    if(target) {
        if(!target->Validate()) return fail();
        const bool visible=target->IsPresentable();
        result.outcome=visible && target->IsBroken()?RN_PRESENT_SURFACE_LOST:RN_PRESENT_SKIPPED;
        if(visible && !target->IsBroken()) {
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.AcquireNext");
                acquired=target->Chain().AcquireNext(100);
            }
            if(device.IsDeviceLost() && !acquired.Frame) return fail();
            auto outcome=ResolveSurfaceOutcome(acquired.Status);
            if(!outcome) return fail();
            if(!acquired.Frame) {
                if(acquired.Status==SwapChainStatus::Success || acquired.Status==SwapChainStatus::Suboptimal) RADRAY_ABORT("Successful surface acquisition has no frame");
                result.outcome=*outcome;
                if(*outcome==RN_PRESENT_SURFACE_LOST) target->MarkBroken();
            }
        }
        if(acquired.Frame) {
            frame.color=acquired.Frame->GetBackBuffer();
            frame.waitToDraw=acquired.Frame->GetWaitToDraw();
            frame.readyToPresent=acquired.Frame->GetReadyToPresent();
        }
    }
    if(measure) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Benchmark.BatchMetrics");
        tape.Measure(result.metrics);
        _terrain.MeasureGeometry(result.metrics);
        _staticGeometry->MeasureGeometry(result.metrics);
    }
    const auto prepared=now();
    _pipeline->SetTape(frame,tape,batch.capture_count!=0);
    auto clear=radray::MakeScopeGuard([&]() noexcept { _pipeline->ClearInput(); });
    bool ran=false;
    {
        std::unique_lock<std::mutex> readbackLock;
        if(batch.capture_count) readbackLock=_pipeline->LockReadbacks();
        ran=_scheduler->RunFrame();
    }
    if(!ran) {
        CheckFailedQueueSubmission(device.IsDeviceLost());
        if(acquired.Frame) target->Chain().DiscardAcquiredFrame(std::move(*acquired.Frame));
        return fail();
    }
    submittedCaptures=true;
    const auto submitted=now();
    result.sequence=_pipeline->SubmittedSequence();
    _pipeline->ClearInput();
    notifySubmitted(result.sequence);
    auto record=radray::MakeScopeGuard([&]() noexcept {
        if(measure) {
            result.wait_ns=waited-start; result.prepare_ns=prepared-waited;
            result.rdg_ns=submitted-prepared; result.present_ns=now()-submitted;
            _pipeline->RecordTelemetry({result.sequence,0,0,0,result.wait_ns,result.prepare_ns,result.rdg_ns,result.present_ns,result.metrics});
        }
    });
    if(acquired.Frame) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Present");
        const auto present=target->Chain().Present(std::move(*acquired.Frame));
        if(device.IsDeviceLost()) return fail();
        auto outcome=ResolveSurfaceOutcome(present.Status,acquired.Status);
        if(!outcome) return fail();
        result.outcome=*outcome;
        if(*outcome==RN_PRESENT_SURFACE_LOST) target->MarkBroken();
    }
    return true;
}
}
