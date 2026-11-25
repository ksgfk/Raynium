#pragma once
#include <raynium/resource_uploads.h>
#include <radray/memory.h>

namespace raynium {
class RendererFrontend;
class Swapchain;
class BatchEnvelope final {
public:
    bool Freeze(const rn_batch_desc&);
    void Reset();
    std::pmr::memory_resource& Memory() { return arena; }
    rn_batch_desc batch{};
    RendererFrontend* frontend{};
    Swapchain* target{};
    uint64_t sequence{};
    PendingResourceOps resources;
    radray::vector<ShadowWrite> shadows;
private:
    radray::HeapMemoryResource heap;
    alignas(std::max_align_t) radray::array<radray::byte,64*1024> initial;
    std::pmr::monotonic_buffer_resource arena{initial.data(),initial.size(),&heap};
};
}
