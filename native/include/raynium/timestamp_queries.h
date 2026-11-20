#pragma once

#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <span>

namespace raynium {
class QueryRegistry final {
public:
    struct Slot { uint64_t sequence{},ticks{}; bool available{}; };
    struct Pool { explicit Pool(uint32_t count):slots(count) {} radray::vector<Slot> slots; };
    struct Write { radray::shared_ptr<Pool> pool; uint32_t slot; };
    bool Create(uint32_t count,uint32_t& id);
    bool Destroy(uint32_t id);
    radray::shared_ptr<Pool> Find(uint32_t id) const;
private:
    uint32_t _next{1};
    radray::unordered_map<uint32_t,radray::shared_ptr<Pool>> _pools;
};

class FrameTimestamps final {
public:
    bool Prepare(radray::gpu::Device&,radray::gpu::CommandQueue&,std::span<const QueryRegistry::Write>);
    void Reset(radray::RDGBuilder&) const;
    void Write(radray::RDGBuilder&,uint32_t physicalIndex) const;
    void Resolve(radray::RDGBuilder&) const;
    void WriteInline(radray::gpu::CommandBuffer& commands,uint32_t index) const;
    void Submitted(uint64_t sequence);
    void Publish(uint64_t completed);
private:
    struct Storage {
        radray::unique_ptr<radray::gpu::QueryPool> queries;
        radray::unique_ptr<radray::gpu::Buffer> readback;
        const uint64_t* mapped;
        ~Storage() { readback->Unmap(); }
    };
    radray::unique_ptr<Storage> _storage;
    radray::gpu::BufferStates _state{radray::gpu::BufferState::Undefined};
    radray::vector<QueryRegistry::Write> _writes;
    uint64_t _sequence{},_mask{};
};
}
