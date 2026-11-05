#pragma once
#include <raynium/render_resources.h>
#include <radray/runtime/material.h>
#include <unordered_map>
#include <memory>

namespace raynium {
struct MeshMaterialBinding {
    radray::unique_ptr<radray::render::Sampler> albedoSampler,lightSampler;
    radray::unique_ptr<radray::render::ShaderParameterSet> parameters;
};
using MaterialBindingMap = std::unordered_map<uint32_t,std::shared_ptr<MeshMaterialBinding>>;

class MeshPassResources final {
public:
    static bool Create(radray::render::Device&,radray::unique_ptr<MeshPassResources>&,radray::string&);
    radray::render::BindingGroupLayout* PassGroupLayout() const { return _objects->passGroupLayout.get(); }
    radray::render::GraphicsPipelineState* Pipeline() const { return _objects->pipeline.get(); }
    bool CreateMaterialBinding(radray::render::Device&,const rn_material_update&,const SharedTextures&,
        MeshMaterialBinding&,radray::MaterialRenderProxy&,radray::string&) const;
private:
    struct Objects {
        radray::unique_ptr<radray::render::Shader> vs,ps;
        radray::unique_ptr<radray::render::BindingGroupLayout> passGroupLayout,materialGroupLayout;
        radray::unique_ptr<radray::render::PipelineLayout> pipelineLayout;
        radray::unique_ptr<radray::render::GraphicsPipelineState> pipeline;
        radray::MaterialResource materialResource;
        std::optional<radray::Material> baseMaterial;
    };
    explicit MeshPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    // Stable storage also keeps baseMaterial's borrowed MaterialResource at a fixed address.
    radray::unique_ptr<Objects> _objects;
};
}
