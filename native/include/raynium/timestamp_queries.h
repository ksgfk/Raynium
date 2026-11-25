#pragma once

#include <raynium/resource_handles.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <span>

namespace raynium {
struct TimestampTarget { radray::IntrusivePtr<QueryPoolStorage> pool; uint32_t slot; };
struct PublishedTimestamp { radray::IntrusivePtr<QueryPoolStorage> pool; uint32_t slot; uint64_t sequence, ticks; };

class FrameTimestamps final {
public:
    bool Prepare(radray::gpu::Device&,radray::gpu::CommandQueue&,std::span<const TimestampTarget>);
    void Reset(radray::RDGBuilder&) const;
    void Write(radray::RDGBuilder&,uint32_t physicalIndex) const;
    void Resolve(radray::RDGBuilder&) const;
    void WriteInline(radray::gpu::CommandBuffer& commands,uint32_t index) const;
    void Submitted(uint64_t sequence);
    bool Collect(uint64_t completed,radray::vector<PublishedTimestamp>& output);
private:
    struct Storage {
        radray::unique_ptr<radray::gpu::QueryPool> queries;
        radray::unique_ptr<radray::gpu::Buffer> readback;
        const uint64_t* mapped;
    };
    radray::unique_ptr<Storage> _storage;
    radray::gpu::BufferStates _state{radray::gpu::BufferState::Undefined};
    radray::vector<TimestampTarget> _writes;
    uint64_t _sequence{},_mask{};
};
}
