#include <raynium/renderer.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>

namespace raynium {
bool Renderer::Create(RayniumRuntime& device,radray::unique_ptr<Renderer>& out,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Renderer.Create");
    if(out) { error="Renderer output occupied"; return false; }
    radray::unique_ptr<GeometryArena> arena;
    if(!GeometryArena::Create(device.GetDevice(),arena,error)) return false;
    radray::unique_ptr<SolidFramePipeline> pipeline;
    if(!SolidFramePipeline::Create(device,pipeline,error)) return false;
    auto* borrowed=pipeline.get();
    auto scheduler=radray::FrameScheduler::Create(device,std::move(pipeline),2).Release();
    if(!scheduler) { error="Cannot create terrain frame scheduler"; return false; }
    out=radray::unique_ptr<Renderer>(new Renderer(device,std::move(scheduler),std::move(arena),borrowed)); return true;
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
    if(!_pipeline->ReadyForFrame(target,error)) return false;
    TerrainFrame prepared;
    if(!_scene.PrepareFrame(target,_textures,frame,_pipeline->Resources(),_pipeline->NextUploads(),prepared,error)) return false;
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
