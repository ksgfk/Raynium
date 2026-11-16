#pragma once
#include <raynium/shader_pack.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/frame_slot.h>
#include <raynium/texture_resources.h>

namespace raynium {
struct OutputBinding {
    radray::IntrusivePtr<TextureResource> texture;
    radray::unique_ptr<radray::render::ShaderParameterSet> parameters;
};
class OutputPassResources final {
public:
    static bool Create(radray::render::Device&,std::span<const radray::byte>,radray::unique_ptr<OutputPassResources>&);
    radray::unique_ptr<radray::render::ShaderParameterSet> CreateParameters(radray::render::Device&) const;
    void AddPass(radray::RDGBuilder&,OutputBinding&,TextureResource&,radray::RDGTextureHandle source,
        radray::RDGTextureHandle destination,uint32_t width,uint32_t height) const;
private:
    struct Objects {
        radray::unique_ptr<ShaderProgram> program;
        radray::unique_ptr<radray::render::BindingGroupLayout> bindings;
        radray::unique_ptr<radray::render::Shader> vs,ps;
        radray::unique_ptr<radray::render::PipelineLayout> layout;
        radray::unique_ptr<radray::render::GraphicsPipelineState> pipeline;
    };
    explicit OutputPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
