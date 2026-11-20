#include <raynium/renderer.h>
#include <raynium/surface_submission.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>
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
    auto vertices=radray::VertexBufferPool::Create(device.GetDevice(),{.VertexStride=28,.PageVertexCapacity=(32u<<20)/28,
        .Layouts=layouts,.Attributes=attributes,.DebugNamePrefix="TerrainVertices"}).Release();
    auto indices=radray::IndexBufferPool::Create(device.GetDevice(),{.PageIndexCapacity=(32u<<20)/sizeof(uint32_t),
        .DebugNamePrefix="TerrainIndices"}).Release();
    if(!vertices || !indices) return false;
    radray::unique_ptr<ModularFramePipeline> pipeline;
    if(!ModularFramePipeline::Create(device,shaders,indirect,pipeline)) return false;
    auto* borrowed=pipeline.get();
    auto scheduler=radray::FrameScheduler::Create(device,std::move(pipeline),2).Release();
    if(!scheduler) return false;
    out=radray::unique_ptr<Renderer>(new Renderer(device,std::move(scheduler),std::move(vertices),std::move(indices),borrowed)); return true;
}
Renderer::~Renderer() noexcept try {
    _scheduler->WaitIdle();
    _terrain.Reset();
} catch(...) {
    AbortUnexpectedException("Renderer::~Renderer");
}
void Renderer::Finish() {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Finish");
    _pipeline->Finish(_device.GetDevice());
    _scheduler->WaitIdle();
}
bool Renderer::WaitSubmit(uint64_t sequence,int64_t timeoutNs,rn_wait_result& result) {
    if(_poisoned) {
        result={};
        if(_device.GetDevice().IsDeviceLost()) result.flags=RN_SUBMIT_FLAG_DEVICE_LOST;
        return false;
    }
    bool success=_pipeline->WaitSubmit(_device.GetDevice(),sequence,timeoutNs,result);
    if(result.flags&RN_SUBMIT_FLAG_DEVICE_LOST) _poisoned=true;
    return success;
}
bool Renderer::CreateQueryPool(uint32_t count,uint32_t& id) {
    return !IsTerminal() && _device.GetDevice().GetCapabilities().Features.TimestampQueries &&
        _device.GetQueue().GetTimestampValidBits() && _queries.Create(count,id);
}
bool Renderer::SyncQueries(uint32_t id,uint32_t first,std::span<rn_query_sample> samples,rn_wait_result& result) {
    auto pool=_queries.Find(id);
    if(!pool || first>pool->slots.size() || samples.size()>pool->slots.size()-first) return false;
    if(!WaitSubmit(0,0,result)) return false;
    for(size_t i=0;i<samples.size();++i) {
        const auto& slot=pool->slots[first+i];
        samples[i]={slot.available?slot.sequence:0,slot.ticks};
    }
    return true;
}
bool Renderer::SubmitFrame(const rn_batch_desc& batch,Swapchain* target,rn_submit_result& result) {
    using namespace radray::gpu;
    auto& device=_device.GetDevice();
    const auto fail=[&] {
        _poisoned=true;
        if(device.IsDeviceLost()) result.flags|=RN_SUBMIT_FLAG_DEVICE_LOST;
        return false;
    };
    if(_poisoned || device.IsDeviceLost()) return fail();
    const bool measure=batch.flags&RN_BATCH_MEASURE;
    const auto now=[measure]()->uint64_t {
        return measure?uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()):0;
    };
    const auto start=now();
    auto& slot=_scheduler->BeginFrame();
    const auto waited=now();
    if(device.IsDeviceLost()) return fail();
    _pipeline->MeasureBatch(slot,measure,target!=nullptr);
    auto& tape=*slot.Emplace<CommandTape>();
    if(!tape.PrepareViews(batch,_terrain,_uploaded,_pipeline->Resources(),slot,_pipeline->UsesIndirect()) ||
       !tape.Decode(batch,_uploaded,_queries,_buffers,_pipelines,device.GetCapabilities().Limits.MaxColorAttachments) || !_pipeline->PrepareTape(slot,tape) ||
       !_buffers.Flush({batch.dirty_ranges,batch.dirty_count})) {
        RADRAY_ERR_LOG("Invalid command tape or readback reservation");
        return fail();
    }
    TerrainFrame frame{};
    frame.textures=&_uploaded;
    const auto backend=device.GetBackend();
    SwapChainAcquireResult acquired{.Status=SwapChainStatus::RetryLater};
    if(target) {
        if(!target->Validate()) return fail();
        result.outcome=RN_PRESENT_SKIPPED;
        if(target->IsPresentable()) {
            acquired=target->Chain().AcquireNext(100);
            if(device.IsDeviceLost() && !acquired.Frame) return fail();
            auto outcome=ResolveSurfaceOutcome(backend,acquired.Status,acquired.NativeStatusCode);
            if(!outcome) return fail();
            if(!acquired.Frame) {
                if(acquired.Status==SwapChainStatus::Success) RADRAY_ABORT("Successful surface acquisition has no frame");
                result.outcome=*outcome;
                if(acquired.Status==SwapChainStatus::RetryLater) target->TimedOut();
            }
        }
        if(acquired.Frame) {
            frame.color=acquired.Frame->GetBackBuffer();
            frame.waitToDraw=acquired.Frame->GetWaitToDraw();
            frame.readyToPresent=acquired.Frame->GetReadyToPresent();
        }
    }
    if(measure) { tape.Measure(result.metrics); _terrain.MeasureGeometry(result.metrics); }
    const auto prepared=now();
    _pipeline->SetTape(frame,tape,_buffers);
    auto clear=radray::MakeScopeGuard([&]() noexcept { _pipeline->ClearInput(); });
    if(!_scheduler->RunFrame()) {
        CheckFailedQueueSubmission(device.IsDeviceLost());
        if(acquired.Frame) target->Chain().DiscardAcquiredFrame(std::move(*acquired.Frame));
        return fail();
    }
    const auto submitted=now();
    result.sequence=_pipeline->SubmittedSequence();
    if(acquired.Frame) {
        const auto present=target->Chain().Present(std::move(*acquired.Frame));
        if(device.IsDeviceLost()) return fail();
        auto outcome=ResolveSurfaceOutcome(backend,present.Status,present.NativeStatusCode,acquired.NativeStatusCode);
        if(!outcome) return fail();
        result.outcome=*outcome;
    }
    if(measure) {
        result.gpu_sequence=_pipeline->GpuSequence(); result.gpu_ns=_pipeline->GpuNanoseconds();
        result.wait_ns=waited-start; result.prepare_ns=prepared-waited;
        result.rdg_ns=submitted-prepared; result.present_ns=now()-submitted;
    }
    return true;
}
}
