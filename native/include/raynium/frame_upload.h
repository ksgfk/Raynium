#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>

#include <radray/runtime/frame_slot.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/transient_parameter_pool.h>
#include <radray/types.h>

namespace raynium {
radray::DynamicBufferResidency StreamResidency(size_t bytes) noexcept;

std::span<const radray::byte> FramePushConstants(
    std::pmr::memory_resource& memory,
    std::span<const radray::byte> bytes,
    size_t required);

class FrameParameterBindings final {
public:
    explicit FrameParameterBindings(radray::FrameSlot& slot);
    std::pmr::vector<radray::TransientParameterBinding>& Group(size_t index) noexcept;

private:
    alignas(radray::TransientParameterBinding) radray::array<radray::byte, 32 * sizeof(radray::TransientParameterBinding)> _storage;
    std::pmr::monotonic_buffer_resource _memory;
    radray::array<std::pmr::vector<radray::TransientParameterBinding>, 2> _groups;
};

class ImmediatePassStateCache final {
public:
    void Invalidate() noexcept;
    void SeedScissor(const radray::Rect& scissor) noexcept;
    void BindPipeline(radray::gpu::GraphicsCommandEncoder& enc, radray::gpu::GraphicsPipelineState* nextPso) noexcept;
    void SetScissor(radray::gpu::GraphicsCommandEncoder& enc, const radray::Rect& next) noexcept;
    void SetStencil(radray::gpu::GraphicsCommandEncoder& enc, uint32_t next) noexcept;
    void BindParameterSet(radray::gpu::GraphicsCommandEncoder& enc, uint32_t group, radray::gpu::ShaderParameterSet* set) noexcept;
    void BindIndexBuffer(radray::gpu::GraphicsCommandEncoder& enc, const radray::gpu::IndexBufferView& next) noexcept;

private:
    radray::gpu::GraphicsPipelineState* _pso{};
    radray::gpu::PipelineLayout* _layout{};
    radray::Rect _scissor{-1, -1, 0, 0};
    uint32_t _stencil{UINT32_MAX};
    radray::array<radray::gpu::ShaderParameterSet*, 2> _sets{};
    radray::gpu::IndexBufferView _indexView{};
    bool _hasIndexView{};
};

void EnsureOrderedBindings(std::span<radray::TransientParameterBinding> group) noexcept;

radray::TransientParameterBinding FrameConstantBinding(uint32_t binding, radray::RDGBufferSlice slice) noexcept;
radray::gpu::VertexBufferView FrameVertexView(radray::RDGBufferSlice slice) noexcept;
radray::gpu::IndexBufferView FrameIndexView(radray::RDGBufferSlice slice) noexcept;
uint64_t FrameIndirectOffset(radray::RDGBufferSlice slice, uint32_t record) noexcept;
}  // namespace raynium
