#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <radray/gpu/types.h>
#include <radray/types.h>
#include <raynium/frame_upload.h>
#include <raynium/resource_uploads.h>
#include <raynium/shader_pack.h>

namespace raynium {
struct RasterPipelineState {
    radray::vector<radray::gpu::VertexBufferLayout> Vertices;
    radray::vector<radray::gpu::VertexAttribute> Attributes;
    radray::gpu::PrimitiveState Primitive = radray::gpu::PrimitiveState::Default();
    std::optional<radray::gpu::DepthStencilState> Depth;
    radray::vector<radray::gpu::ColorTargetState> Colors;
};

class RasterProgram final {
public:
    radray::vector<radray::byte> ExternalBytes;
    uint32_t PushBytes{};
    radray::unique_ptr<ShaderProgram> Program;
    radray::unique_ptr<radray::gpu::Shader> Vs;
    radray::unique_ptr<radray::gpu::Shader> Ps;
    radray::array<radray::shared_ptr<radray::gpu::BindingGroupLayout>, 2> Groups;
    radray::shared_ptr<radray::gpu::BindingGroupLayout> DrawBasesLayout;
    radray::unique_ptr<radray::gpu::PipelineLayout> Layout;
    radray::array<radray::vector<uint32_t>, 2> Required;
    radray::vector<uint32_t> Samplers;
    radray::array<uint64_t, 2> RequiredMask{};
    uint64_t SamplerMask{};
    bool HasHighBindings{};

    bool RequiresBinding(uint32_t group, uint32_t slot) const noexcept;
    bool HasSampler(uint32_t slot) const noexcept;
};

class RenderProgramLibrary final {
public:
    static bool Create(std::span<const radray::byte> bytes, radray::unique_ptr<RenderProgramLibrary>& result);
    RasterProgram* GetProgram(
        radray::gpu::Device& device,
        std::string_view name,
        std::span<const radray::byte> external = {});

private:
    explicit RenderProgramLibrary(radray::vector<radray::byte> bytes);

    radray::vector<radray::byte> _bytes;
    radray::vector<ShaderPackEntry> _entries;
    radray::unordered_map<radray::string, radray::unique_ptr<RasterProgram>> _programs;
};
}  // namespace raynium
