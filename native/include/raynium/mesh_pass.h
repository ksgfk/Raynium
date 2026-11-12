#pragma once
#include <radray/types.h>
#include <raynium/render_resources.h>
#include <raynium/texture_resources.h>
#include <raynium/shader_pack.h>
#include <span>
#include <utility>

namespace raynium {
enum TerrainPass : uint32_t { Solid=0, Cutout=1, Translucent=2, OitBounds=3, OitTransmittance=4, OitAccumulate=5, OitCull=6, OitComposite=7 };
struct MeshMaterialBinding {
    uint64_t sortKey{};
    radray::array<radray::shared_ptr<UploadedTexture>,2> textures;
    radray::unique_ptr<radray::render::Sampler> albedoSampler,lightSampler;
    radray::unique_ptr<radray::render::ShaderParameterSet> parameters;
};

class MeshPassResources final {
public:
    static bool Create(radray::render::Device&,std::span<const radray::byte>,radray::unique_ptr<MeshPassResources>&);
    radray::render::BindingGroupLayout* PassGroupLayout() const { return _objects->passGroupLayout.get(); }
    radray::render::GraphicsPipelineState* Pipeline() const { return _objects->programs[Solid].pipeline.get(); }
    radray::render::GraphicsPipelineState* Pipeline(uint32_t pass,radray::render::TextureFormat=radray::render::TextureFormat::BGRA8_UNORM,bool flipY=false) const {
        const auto& p=_objects->programs[pass]; return flipY && pass<OitCull?p.flippedPipeline.get():p.pipeline.get();
    }
    bool PrepareOutput(radray::render::Device&,radray::render::TextureFormat);
    radray::render::BindingGroupLayout* OitGroupLayout() const { return _objects->oitGroupLayout.get(); }
    bool CreateMaterialBinding(radray::render::Device&,const rn_render_frame_desc&,const UploadedTextures&,
        MeshMaterialBinding&) const;
private:
    struct Program {
        radray::unique_ptr<ShaderProgram> program;
        radray::unique_ptr<radray::render::PipelineLayout> pipelineLayout;
        radray::unique_ptr<radray::render::Shader> vs,ps;

        radray::unique_ptr<radray::render::GraphicsPipelineState> pipeline,flippedPipeline;
    };
    struct Objects {
        radray::unique_ptr<radray::render::BindingGroupLayout> passGroupLayout,materialGroupLayout,oitGroupLayout;
        radray::array<Program,8> programs;
    };
    explicit MeshPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
