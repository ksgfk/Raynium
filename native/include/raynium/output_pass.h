#pragma once

#include <cstdint>
#include <span>

#include <radray/nullable.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/resource_uploads.h>
#include <raynium/shader_pack.h>

namespace raynium {
class OutputPassResources final {
public:
    static bool Create(
        radray::gpu::Device& device,
        std::span<const radray::byte> shaders,
        radray::unique_ptr<OutputPassResources>& out,
        radray::Nullable<radray::gpu::PipelineCache*> cache = nullptr);
    void AddPass(
        radray::RDGBuilder& graph,
        radray::RDGTextureHandle source,
        radray::RDGTextureHandle destination,
        uint32_t width,
        uint32_t height,
        uint32_t sourceMip = 0,
        bool flip = false) const;

private:
    struct Objects {
        radray::unique_ptr<ShaderProgram> Program;
        radray::shared_ptr<radray::gpu::BindingGroupLayout> Bindings;
        radray::unique_ptr<radray::gpu::Shader> Vs;
        radray::unique_ptr<radray::gpu::Shader> Ps;
        radray::unique_ptr<radray::gpu::PipelineLayout> Layout;
        radray::unique_ptr<radray::gpu::GraphicsPipelineState> Pipeline;
    };

    explicit OutputPassResources(radray::unique_ptr<Objects> objects);

    radray::unique_ptr<Objects> _objects;
};
}  // namespace raynium
