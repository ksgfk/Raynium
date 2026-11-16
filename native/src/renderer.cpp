#include <raynium/renderer.h>
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
    using namespace radray::render;
    const auto& caps=device.GetDevice().GetCapabilities();
    bool indirect=false;
    if(!SelectTerrainSubmission(flags,caps,indirect)) return false;
    const radray::array layouts{VertexBufferLayout{0,28,VertexStepMode::Vertex},VertexBufferLayout{1,16,VertexStepMode::Instance}};
    const radray::array attributes{radray::VertexInputAttribute{0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},
        radray::VertexInputAttribute{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        radray::VertexInputAttribute{0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},
        radray::VertexInputAttribute{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        radray::VertexInputAttribute{1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},
        radray::VertexInputAttribute{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    auto vertices=radray::VertexBufferPool::Create(device.GetDevice(),{.VertexStride=28,.PageVertexCapacity=(32u<<20)/28,
        .Layouts=layouts,.Attributes=attributes,.DebugNamePrefix="TerrainVertices"}).Release();
    auto indices=radray::IndexBufferPool::Create(device.GetDevice(),{.PageIndexCapacity=(32u<<20)/sizeof(uint32_t),
        .DebugNamePrefix="TerrainIndices"}).Release();
    if(!vertices || !indices) return false;
    radray::unique_ptr<SolidFramePipeline> pipeline;
    if(!SolidFramePipeline::Create(device,shaders,indirect,pipeline)) return false;
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
    _pipeline->Finish();
    _scheduler->WaitIdle();
}
bool Renderer::Present(Swapchain& target,const rn_render_frame_desc& frame,rn_present_result& result) {
    using namespace radray::render;
    auto markFrame=radray::MakeScopeGuard([]() noexcept { RADRAY_PROFILE_FRAME(); });
    RADRAY_PROFILE_SCOPE_N("Raynium.Present");
    if((frame.flags&RN_FRAME_WIREFRAME) && !SupportsWireframe()) { RADRAY_ERR_LOG("Terrain wireframe is unavailable on this device"); return false; }
    if(!target.Validate(frame.output_sequence)) return false;
    const bool measure=(frame.flags&RN_FRAME_MEASURE)!=0;
    const auto now=[&]() -> uint64_t { return measure?uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()):0; };
    const auto start=now();
    auto& slot=_scheduler->BeginFrame();
    _pipeline->BeginMeasurements(slot,measure);
    const auto waited=now();
    auto clearScene=radray::MakeScopeGuard([&]() noexcept { _pipeline->ClearSceneInput(slot); });
    TerrainFrame prepared;
    auto uploads=_pipeline->GetUploads(slot);
    if(!_terrain.PrepareFrame(_uploaded,frame,_pipeline->Resources(),uploads,prepared)) return false;
    prepared.textures=&_uploaded;
    const auto outputDesc=target.Chain().GetDesc();
    const bool offscreen=frame.render_width || frame.render_height;
    const auto limit=_device.GetDevice().GetCapabilities().Limits.MaxTexture2DDimension;
    if(((frame.flags&RN_FRAME_WORLD_ONLY) && !offscreen) || (offscreen && (!frame.render_width || !frame.render_height || frame.render_width>limit || frame.render_height>limit))) {
        RADRAY_ERR_LOG("Invalid offscreen render extent: {}x{}",frame.render_width,frame.render_height); return false;
    }
    prepared.width=offscreen?frame.render_width:outputDesc.Width; prepared.height=offscreen?frame.render_height:outputDesc.Height;
    if(!_pipeline->PrepareCapture(slot,frame,prepared.width,prepared.height)) return false;
    if(!_pipeline->PrepareGui(slot,_uploaded,frame,prepared.width,prepared.height)) return false;
    if(!_pipeline->PrepareScene(slot,_uploaded,frame)) return false;
    const auto preparedAt=now();
    SwapChainAcquireResult acquired{.Status=SwapChainStatus::RetryLater};
    if(!offscreen && target.IsPresentable()) {
        acquired=target.Chain().AcquireNext(100);
        if(acquired.Status==SwapChainStatus::RetryLater) {
            target.TimedOut();
            RADRAY_DEBUG_LOG("Raynium swapchain acquire retry count={}",target.Timeouts());
        }
    }
    if(acquired.Status==SwapChainStatus::Error) { RADRAY_ERR_LOG("Swapchain acquisition failed; close the renderer"); return false; }
    if(acquired.Frame) {
        prepared.color=acquired.Frame->GetBackBuffer();
        prepared.waitToDraw=acquired.Frame->GetWaitToDraw(); prepared.readyToPresent=acquired.Frame->GetReadyToPresent();
    } else if(acquired.Status==SwapChainStatus::Success) RADRAY_ABORT("Successful swapchain acquisition has no frame");
    _pipeline->SetInput(prepared);
    auto clear=radray::MakeScopeGuard([&]() noexcept { _pipeline->ClearInput(); });
    const auto rdgStart=now();
    _scheduler->RunFrame();
    const auto rdgEnd=now();
    auto outcome=acquired.Status==SwapChainStatus::RequireRecreate?RN_PRESENT_RECREATE:RN_PRESENT_SKIPPED;
    if(acquired.Frame) {
        const auto presented=target.Chain().Present(std::move(*acquired.Frame));
        if(presented.Status==SwapChainStatus::Error) { RADRAY_ERR_LOG("Swapchain presentation failed; close the renderer"); return false; }
        outcome=presented.Status==SwapChainStatus::Success?RN_PRESENTED:
            presented.Status==SwapChainStatus::RequireRecreate?RN_PRESENT_RECREATE:RN_PRESENT_SKIPPED;
    }
    target.Commit(frame.output_sequence);
    result={sizeof(result),outcome,target.Timeouts(),measure?_pipeline->GpuSequence():0,measure?_pipeline->GpuNanoseconds():0,
        waited-start,preparedAt-waited,rdgEnd-rdgStart,now()-rdgEnd,_pipeline->Measurements(slot)};
    if(measure) _terrain.MeasureGeometry(result.metrics);
    return true;
}
}
