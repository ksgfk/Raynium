#pragma once
#include <raynium/solid_pipeline.h>

namespace raynium {
class Renderer final {
public:
    static bool Create(RayniumRuntime&,radray::unique_ptr<Renderer>&,radray::string&);
    bool Render(RenderTarget&,const rn_render_frame_desc&,radray::string&);
    bool Finish(radray::string&);
    void AttachTextures(SharedTextures* textures) noexcept;
    RayniumRuntime& Runtime() { return _device; }
private:
    Renderer(RayniumRuntime& device,std::unique_ptr<radray::Renderer> renderer,SolidRenderPipeline* pipeline)
        :_device(device),_renderer(std::move(renderer)),_pipeline(pipeline) {}
    RayniumRuntime& _device;
    std::unique_ptr<radray::Renderer> _renderer;
    SolidRenderPipeline* _pipeline;
    radray::Scene _scene;
    SharedTextures* _textures{};
    uint64_t _textureVersion{};
};
}
