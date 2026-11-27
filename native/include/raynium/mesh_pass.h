#pragma once

#include <cstdint>
#include <span>

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <raynium/render_resources.h>
#include <raynium/resource_uploads.h>
#include <raynium/shader_pack.h>

namespace raynium {
enum TerrainPass : uint32_t {
    Solid = 0,
    Cutout = 1,
    Translucent = 2,
    OitBounds = 3,
    OitTransmittance = 4,
    OitAccumulate = 5,
    Wireframe = 6
};

class MeshPassResources final {
public:
    static bool Create(
        radray::gpu::Device& device,
        std::span<const radray::byte> shaders,
        radray::unique_ptr<MeshPassResources>& output,
        radray::Nullable<radray::gpu::PipelineCache*> cache = nullptr);
    const radray::shared_ptr<radray::gpu::BindingGroupLayout>& PassGroupLayout() const noexcept;
    radray::gpu::GraphicsPipelineState* Pipeline() const noexcept;
    bool SupportsWireframe() const noexcept;
    radray::gpu::GraphicsPipelineState* WireframePipeline() const noexcept;
    radray::gpu::GraphicsPipelineState* Pipeline(uint32_t pass, bool wireframe = false) const noexcept;
    const radray::shared_ptr<radray::gpu::BindingGroupLayout>& OitGroupLayout() const noexcept;
    const radray::shared_ptr<radray::gpu::BindingGroupLayout>& MaterialGroupLayout() const noexcept;

private:
    struct Program {
        radray::unique_ptr<ShaderProgram> Shader;
        radray::unique_ptr<radray::gpu::PipelineLayout> PipelineLayout;
        radray::unique_ptr<radray::gpu::Shader> Vs;
        radray::unique_ptr<radray::gpu::Shader> Ps;
        radray::unique_ptr<radray::gpu::GraphicsPipelineState> Pipeline;
    };

    struct Objects {
        radray::shared_ptr<radray::gpu::BindingGroupLayout> PassGroupLayout;
        radray::shared_ptr<radray::gpu::BindingGroupLayout> MaterialGroupLayout;
        radray::shared_ptr<radray::gpu::BindingGroupLayout> OitGroupLayout;
        radray::array<Program, 6> Programs;
        radray::unique_ptr<radray::gpu::GraphicsPipelineState> Wireframe;
    };

    explicit MeshPassResources(radray::unique_ptr<Objects> objects);

    radray::unique_ptr<Objects> _objects;
};
}  // namespace raynium
