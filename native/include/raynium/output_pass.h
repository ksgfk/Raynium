#pragma once
#include <raynium/shader_pack.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/frame_slot.h>
#include <raynium/texture_registry.h>

namespace raynium {
class OutputPassResources final {
public:
    static bool Create(radray::gpu::Device&,std::span<const radray::byte>,radray::unique_ptr<OutputPassResources>&,
        radray::Nullable<radray::gpu::PipelineCache*> cache=nullptr);
    void AddPass(radray::RDGBuilder&,radray::RDGTextureHandle source,
        radray::RDGTextureHandle destination,uint32_t width,uint32_t height,uint32_t sourceMip=0,bool flip=false) const;
private:
    struct Objects {
        radray::unique_ptr<ShaderProgram> program;
        radray::shared_ptr<radray::gpu::BindingGroupLayout> bindings;
        radray::unique_ptr<radray::gpu::Shader> vs,ps;
        radray::unique_ptr<radray::gpu::PipelineLayout> layout;
        radray::unique_ptr<radray::gpu::GraphicsPipelineState> pipeline;
    };
    explicit OutputPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
