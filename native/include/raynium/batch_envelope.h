#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>

#include <radray/memory.h>
#include <radray/types.h>
#include <raynium/resource_uploads.h>

namespace raynium {
class RendererFrontend;
class Swapchain;

class BatchEnvelope final {
public:
    bool Freeze(const rn_batch_desc& input);
    void Reset();
    std::pmr::memory_resource& Memory() noexcept;

    rn_batch_desc Batch{};
    RendererFrontend* Frontend{};
    Swapchain* Target{};
    uint64_t Sequence{};
    PendingResourceOps Resources;
    radray::vector<ShadowWrite> Shadows;

private:
    static constexpr size_t kInitialArenaBytes = 64 * 1024;

    radray::HeapMemoryResource _heap;
    alignas(std::max_align_t) radray::array<radray::byte, kInitialArenaBytes> _initial;
    std::pmr::monotonic_buffer_resource _arena{_initial.data(), _initial.size(), &_heap};
};
}  // namespace raynium
