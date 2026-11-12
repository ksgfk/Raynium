#include <raynium/renderer.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace raynium {
bool Renderer::Create(RayniumRuntime& device,std::span<const radray::byte> shaders,uint32_t flags,radray::unique_ptr<Renderer>& out,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Create");
    if(out) { error="Renderer output occupied"; return false; }
    using namespace radray::render;
    const auto& caps=device.GetDevice().GetCapabilities();
    bool indirect=false; radray::string reason;
    if(!SelectTerrainSubmission(flags,caps,indirect,reason)) { error=radray::string{"Terrain submission unavailable: "}+reason; return false; }
    RADRAY_INFO_LOG("Terrain submission requested={} actual={} indirect={} first_instance={} max_native_records={} fallback={}",
        flags==RN_RENDERER_DIRECT?"direct":flags==RN_RENDERER_INDIRECT?"indirect":"auto",indirect?"indirect":"direct",
        caps.Features.IndirectDraw,caps.Features.IndirectFirstInstance,caps.Limits.MaxDrawIndirectCount,
        flags==0 && !indirect?reason.c_str():"none");
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
    if(!vertices || !indices) { error="Cannot create terrain geometry pools"; return false; }
    radray::unique_ptr<SolidFramePipeline> pipeline;
    if(!SolidFramePipeline::Create(device,shaders,indirect,pipeline,error)) return false;
    auto* borrowed=pipeline.get();
    auto scheduler=radray::FrameScheduler::Create(device,std::move(pipeline),2).Release();
    if(!scheduler) { error="Cannot create terrain frame scheduler"; return false; }
    out=radray::unique_ptr<Renderer>(new Renderer(device,std::move(scheduler),std::move(vertices),std::move(indices),borrowed)); return true;
}
Renderer::~Renderer() noexcept {
    _scheduler->WaitIdle();
    _terrain.Reset();
}
bool Renderer::Finish(radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Finish");
    if(!_pipeline->Finish(error)) return false;
    _scheduler->WaitIdle(); return true;
}
bool Renderer::Render(RenderTarget& target,const rn_render_frame_desc& frame,radray::string& error) {
    auto markFrame=radray::MakeScopeGuard([]() noexcept { RADRAY_PROFILE_FRAME(); });
    RADRAY_PROFILE_SCOPE_N("Raynium.Render");
    RADRAY_PROFILE_PLOT("Raynium.Frame.Sequence",double(frame.output_sequence));
    RADRAY_PROFILE_PLOT("Raynium.Visible", double(frame.visible_count));
    RADRAY_PROFILE_PLOT("Raynium.MeshUpdates", double(frame.mesh_count));
    if(!frame.output_sequence || frame.output_sequence==UINT64_MAX || frame.output_sequence!=target.Submitted()+1) {
        error="Output sequence must start at 1 and increase by one"; return false;
    }
    if(_textures ? !_textures->Validate(frame.input_sequence,frame.changed_textures,error) : (frame.input_sequence || frame.changed_textures)) {
        if(error.empty()) error="Frame has inputs without a shared texture set"; return false;
    }
    auto& slot=_scheduler->BeginFrame();
    TerrainFrame prepared;
    if(!_terrain.PrepareFrame(target,_textures,frame,_pipeline->Resources(),_pipeline->GetUploads(slot),prepared,error)) return false;
    if((frame.flags&RN_FRAME_PROFILE) && frame.output_sequence%120==0)
        RADRAY_INFO_LOG("Terrain memory resident_geometry_bytes={} oit_attachment_bytes_per_slot={} target={}x{}",
            _terrain.ResidentBytes(),prepared.hasTranslucent && (frame.flags&RN_FRAME_IMPROVED_TRANSPARENCY)?uint64_t(target.Width())*target.Height()*56:0,
            target.Width(),target.Height());
    _pipeline->SetInput(prepared);
    auto endFrame=radray::MakeScopeGuard([&]() noexcept { _pipeline->ClearInput(); });
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.RunFrame");
        _scheduler->RunFrame();
    }
    target.Commit(frame.output_sequence); if(_textures) _textures->Commit(frame.changed_textures);
    return true;
}
}
