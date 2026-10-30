#pragma once
#include <raynium/mesh_pass.h>

namespace raynium {
class SolidPassProcessor final : public radray::MeshPassProcessor {
public:
    explicit SolidPassProcessor(radray::unique_ptr<MeshPassResources> resources):_resources(std::move(resources)) {}
    uint32_t textureCount{};
    bool Validate(const radray::MeshBatch&,const radray::MaterialDescriptor&,std::string&) const override;
    MeshPassResources& Resources() { return *_resources; }
private:
    radray::unique_ptr<MeshPassResources> _resources;
};
struct SolidFrame { RenderTarget& target; SharedTextures* textures; const rn_render_frame_desc& frame; };
class SolidRenderPipeline final : public radray::IRenderPipeline {
public:
    static bool Create(RayniumRuntime&,std::unique_ptr<SolidRenderPipeline>&,radray::string&);
    void TextureCount(uint32_t count) { _processor.textureCount=count; }
    const radray::MeshPassProcessor& Processor() const override { return _processor; }
    bool BuildGraph(radray::GraphBuilder&,radray::FrameContext&,radray::PreparedMeshPass&,radray::Nullable<const void*>,std::string&) override;
private:
    SolidRenderPipeline(RayniumRuntime& device,radray::unique_ptr<MeshPassResources> resources):_device(device),_processor(std::move(resources)) {}
    RayniumRuntime& _device;
    SolidPassProcessor _processor;
};
}
