#include <raynium/renderer.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>

namespace raynium {
bool Renderer::Create(RayniumRuntime& device,radray::unique_ptr<Renderer>& out,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Create");
    if(out) { error="Renderer output occupied"; return false; }
    using namespace radray::render;
    const radray::array layouts{VertexBufferLayout{0,28,VertexStepMode::Vertex},VertexBufferLayout{1,16,VertexStepMode::Instance}};
    const radray::array attributes{radray::VertexInputAttribute{0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},
        radray::VertexInputAttribute{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        radray::VertexInputAttribute{0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},
        radray::VertexInputAttribute{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        radray::VertexInputAttribute{1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},
        radray::VertexInputAttribute{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    auto vertices=radray::VertexBufferPool::Create(device.GetDevice(),{.VertexStride=28,.PageVertexCapacity=(32u<<20)/28,
        .Layouts=layouts,.Attributes=attributes,.DebugNamePrefix="TerrainVertices"}).Release();
    auto indices=radray::IndexBufferPool::Create(device.GetDevice(),{.DebugNamePrefix="TerrainIndices"}).Release();
    if(!vertices || !indices) { error="Cannot create terrain geometry pools"; return false; }
    radray::unique_ptr<SolidFramePipeline> pipeline;
    if(!SolidFramePipeline::Create(device,pipeline,error)) return false;
    auto* borrowed=pipeline.get();
    auto scheduler=radray::FrameScheduler::Create(device,std::move(pipeline),2).Release();
    if(!scheduler) { error="Cannot create terrain frame scheduler"; return false; }
    out=radray::unique_ptr<Renderer>(new Renderer(device,std::move(scheduler),std::move(vertices),std::move(indices),borrowed)); return true;
}
Renderer::~Renderer() noexcept {
    _scheduler->WaitIdle();
    _scene.Reset();
}
bool Renderer::Finish(radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Finish");
    if(!_pipeline->Finish(error)) return false;
    _scheduler->WaitIdle(); return true;
}
bool Renderer::Render(RenderTarget& target,const rn_render_frame_desc& frame,radray::string& error) {
    auto markFrame=radray::MakeScopeGuard([]() noexcept { RADRAY_PROFILE_FRAME(); });
    RADRAY_PROFILE_SCOPE_N("Raynium.Render");
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
    if(!_scene.PrepareFrame(target,_textures,frame,_pipeline->Resources(),_pipeline->GetUploads(slot),prepared,error)) return false;
    RADRAY_PROFILE_PLOT("Raynium.Cache.DirtyBatches", double(_scene.Scene().GetDirtyBatches().size()));
    _pipeline->BeginFrame(prepared,&_scene.MaterialBindings());
    auto endFrame=radray::MakeScopeGuard([&]() noexcept { _pipeline->EndFrame(); });
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.RunFrame");
        _scheduler->RunFrame(_scene.Scene());
    }
    target.Commit(frame.output_sequence); if(_textures) _textures->Commit(frame.changed_textures);
    return true;
}
}
