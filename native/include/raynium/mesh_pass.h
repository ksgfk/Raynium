#pragma once
#include <raynium/render_resources.h>
#include <raynium/shader_pack.h>
#include <memory>

namespace raynium {
enum TerrainPass : uint32_t { Solid=0, Cutout=1, Translucent=2, OitBounds=3, OitTransmittance=4, OitAccumulate=5, OitCull=6, OitComposite=7 };
struct MeshMaterialBinding {
    rn_material_mode mode{};
    uint64_t sortKey{};
    radray::unique_ptr<radray::render::Sampler> albedoSampler,lightSampler;
    radray::unique_ptr<radray::render::ShaderParameterSet> parameters;
};

class MeshPassResources final {
public:
    static bool Create(radray::render::Device&,std::span<const radray::byte>,radray::unique_ptr<MeshPassResources>&,radray::string&);
    radray::render::BindingGroupLayout* PassGroupLayout() const { return _objects->passGroupLayout.get(); }
    radray::render::GraphicsPipelineState* Pipeline() const { return _objects->programs[Solid].pipeline.get(); }
    radray::render::GraphicsPipelineState* Pipeline(uint32_t pass) const { return _objects->programs[pass].pipeline.get(); }
    radray::render::BindingGroupLayout* OitGroupLayout() const { return _objects->oitGroupLayout.get(); }
    bool CreateMaterialBinding(radray::render::Device&,const rn_material_update&,const SharedTextures&,
        MeshMaterialBinding&,radray::string&) const;
private:
    struct Program {
        std::unique_ptr<ShaderProgram> program;
        radray::unique_ptr<radray::render::PipelineLayout> pipelineLayout;
        radray::unique_ptr<radray::render::Shader> vs,ps;

        radray::unique_ptr<radray::render::GraphicsPipelineState> pipeline;
    };
    struct Objects {
        radray::unique_ptr<radray::render::BindingGroupLayout> passGroupLayout,materialGroupLayout,oitGroupLayout;
        std::array<Program,8> programs;
    };
    explicit MeshPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
