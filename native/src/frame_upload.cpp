#include <raynium/frame_upload.h>

#include <algorithm>

#include <radray/logger.h>
#include <raynium/frame_plan.h>

namespace raynium {
radray::DynamicBufferResidency StreamResidency(size_t bytes) noexcept {
    constexpr size_t kDeviceResidencyLimit = 64u << 10;
    return bytes > kDeviceResidencyLimit ? radray::DynamicBufferResidency::HostOnly : radray::DynamicBufferResidency::PreferDevice;
}

std::span<const radray::byte> FramePushConstants(
    std::pmr::memory_resource& memory,
    std::span<const radray::byte> bytes,
    size_t required) {
    if (bytes.size() >= required) return bytes.first(required);
    auto padded = FrameArray<radray::byte>(memory, required);
    std::copy(bytes.begin(), bytes.end(), padded.begin());
    return padded;
}

FrameParameterBindings::FrameParameterBindings(radray::FrameSlot& slot)
    : _memory(_storage.data(), _storage.size(), &slot.GetScratchMemoryResource()),
      _groups{
          std::pmr::vector<radray::TransientParameterBinding>(&_memory),
          std::pmr::vector<radray::TransientParameterBinding>(&_memory),
      } {
    for (auto& group : _groups) group.reserve(16);
}

std::pmr::vector<radray::TransientParameterBinding>& FrameParameterBindings::Group(size_t index) noexcept {
    return _groups[index];
}

void ImmediatePassStateCache::Invalidate() noexcept {
    *this = ImmediatePassStateCache{};
}

void ImmediatePassStateCache::SeedScissor(const radray::Rect& scissor) noexcept {
    _scissor = scissor;
}

void ImmediatePassStateCache::BindPipeline(radray::gpu::GraphicsCommandEncoder& enc, radray::gpu::GraphicsPipelineState* nextPso) noexcept {
    if (_pso == nextPso) return;
    auto* nextLayout = nextPso ? nextPso->GetLayout().Get() : nullptr;
    const bool sameLayout = _pso != nullptr && nextLayout != nullptr && _layout == nextLayout;
    enc.BindGraphicsPipelineState(nextPso);
    _pso = nextPso;
    _layout = nextLayout;
    if (!sameLayout) _sets = {};
}

void ImmediatePassStateCache::SetScissor(radray::gpu::GraphicsCommandEncoder& enc, const radray::Rect& next) noexcept {
    if (_scissor.X == next.X && _scissor.Y == next.Y && _scissor.Width == next.Width && _scissor.Height == next.Height) return;
    enc.SetScissor(next);
    _scissor = next;
}

void ImmediatePassStateCache::SetStencil(radray::gpu::GraphicsCommandEncoder& enc, uint32_t next) noexcept {
    if (_stencil == next) return;
    enc.SetStencilReference(uint8_t(next));
    _stencil = next;
}

void ImmediatePassStateCache::BindParameterSet(radray::gpu::GraphicsCommandEncoder& enc, uint32_t group, radray::gpu::ShaderParameterSet* set) noexcept {
    if (group < _sets.size()) {
        if (_sets[group] == set) return;
        _sets[group] = set;
    }
    enc.BindShaderParameterSet(group, set);
}

void ImmediatePassStateCache::BindIndexBuffer(radray::gpu::GraphicsCommandEncoder& enc, const radray::gpu::IndexBufferView& next) noexcept {
    if (_hasIndexView && _indexView.Target == next.Target && _indexView.Offset == next.Offset && _indexView.Stride == next.Stride) return;
    enc.BindIndexBuffer(next);
    _indexView = next;
    _hasIndexView = true;
}

void EnsureOrderedBindings(std::span<radray::TransientParameterBinding> group) noexcept {
    if (group.size() <= 1) return;
    bool sorted = true;
    for (size_t i = 1; i < group.size(); ++i) {
        const auto& a = group[i - 1];
        const auto& b = group[i];
        if (a.BindingId > b.BindingId || (a.BindingId == b.BindingId && a.ArrayElement > b.ArrayElement)) {
            sorted = false;
            break;
        }
    }
    if (!sorted) {
        std::sort(group.begin(), group.end(), [](const radray::TransientParameterBinding& a, const radray::TransientParameterBinding& b) noexcept {
            return a.BindingId != b.BindingId ? a.BindingId < b.BindingId : a.ArrayElement < b.ArrayElement;
        });
    }
}

radray::TransientParameterBinding FrameConstantBinding(uint32_t binding, radray::RDGBufferSlice slice) noexcept {
    return {binding, 0, radray::gpu::ShaderBufferBinding{slice.Buffer.Get(), {slice.Offset, slice.Size}}};
}

radray::gpu::VertexBufferView FrameVertexView(radray::RDGBufferSlice slice) noexcept {
    return {slice.Buffer.Get(), slice.Offset, slice.Size};
}

radray::gpu::IndexBufferView FrameIndexView(radray::RDGBufferSlice slice) noexcept {
    if (slice.Offset > UINT32_MAX) RADRAY_ABORT("Index slice offset cannot be represented: {}", slice.Offset);
    return {slice.Buffer.Get(), uint32_t(slice.Offset), 4};
}

uint64_t FrameIndirectOffset(radray::RDGBufferSlice slice, uint32_t record) noexcept {
    constexpr auto kStride = sizeof(radray::gpu::DrawIndexedIndirectArguments);
    static_assert(kStride == 20);
    const auto offset = uint64_t(record) * kStride;
    if (!slice.Buffer || offset > slice.Size || slice.Size - offset < kStride || slice.Offset > UINT64_MAX - offset) {
        RADRAY_ABORT("Invalid indirect slice record: record={} size={}", record, slice.Size);
    }
    return slice.Offset + offset;
}
}  // namespace raynium
