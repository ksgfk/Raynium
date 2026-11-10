#pragma once
#include <raynium/solid_pipeline.h>

namespace raynium {
class Renderer final {
public:
    ~Renderer() noexcept;
    static bool Create(RayniumRuntime&,std::span<const radray::byte>,radray::unique_ptr<Renderer>&,radray::string&);
    bool Render(RenderTarget&,const rn_render_frame_desc&,radray::string&);
    bool Finish(radray::string&);
    void AttachTextures(SharedTextures* textures) noexcept {
        if(_textures!=textures) _scene.DetachTextures();
        _textures=textures;
    }
    RayniumRuntime& Runtime() { return _device; }
private:
    Renderer(RayniumRuntime& device,radray::unique_ptr<radray::FrameScheduler> scheduler,
        radray::unique_ptr<radray::VertexBufferPool> vertices,radray::unique_ptr<radray::IndexBufferPool> indices,SolidFramePipeline* pipeline)
        :_device(device),_scheduler(std::move(scheduler)),_scene(device,std::move(vertices),std::move(indices)),_pipeline(pipeline) {}
    RayniumRuntime& _device;
    radray::unique_ptr<radray::FrameScheduler> _scheduler;
    TerrainScene _scene;
    SolidFramePipeline* _pipeline;
    SharedTextures* _textures{};
};
}
