#pragma once

#include <cstdint>
#include <span>

#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/resource_handles.h>

namespace raynium {
struct TimestampTarget {
    radray::IntrusivePtr<QueryPoolStorage> Pool;
    uint32_t Slot{};
};

struct PublishedTimestamp {
    radray::IntrusivePtr<QueryPoolStorage> Pool;
    uint32_t Slot{};
    uint64_t Sequence{};
    uint64_t Ticks{};
};

class FrameTimestamps final {
public:
    bool Prepare(radray::gpu::Device& device, radray::gpu::CommandQueue& queue, std::span<const TimestampTarget> writes);
    void Reset(radray::RDGBuilder& graph) const;
    void Write(radray::RDGBuilder& graph, uint32_t physicalIndex) const;
    void Resolve(radray::RDGBuilder& graph) const;
    void WriteInline(radray::gpu::CommandBuffer& commands, uint32_t index) const;
    void Submitted(uint64_t sequence);
    bool Collect(uint64_t completed, radray::vector<PublishedTimestamp>& output);

private:
    struct Storage {
        radray::unique_ptr<radray::gpu::QueryPool> Queries;
        radray::unique_ptr<radray::gpu::Buffer> Readback;
        const uint64_t* Mapped{};
    };

    radray::unique_ptr<Storage> _storage;
    radray::gpu::BufferStates _state{radray::gpu::BufferState::Undefined};
    radray::vector<TimestampTarget> _writes;
    uint64_t _sequence{};
    uint64_t _mask{};
};
}  // namespace raynium
