#pragma once
#include <raynium/bridge.h>
#include <radray/memory.h>
#include <radray/types.h>

namespace raynium {

struct PacketQuery { uint32_t pool, slot; };
bool InspectCommandStream(const rn_batch_desc&, radray::vector<uint64_t>&, radray::vector<PacketQuery>&);

class FramePacket final {
public:
    static radray::unique_ptr<FramePacket> Create(const rn_batch_desc&);
    const rn_batch_desc batch;
    const radray::vector<uint64_t> captures;
    const radray::vector<PacketQuery> queries;

private:
    struct Storage {
        radray::HeapMemoryResource heap;
        std::pmr::monotonic_buffer_resource arena{&heap};
    };
    FramePacket(radray::unique_ptr<Storage> storage, const rn_batch_desc& desc,
                radray::vector<uint64_t> captureIds, radray::vector<PacketQuery> writes)
        : batch(desc), captures(std::move(captureIds)), queries(std::move(writes)), _storage(std::move(storage)) {}
    radray::unique_ptr<Storage> _storage;
};
}
