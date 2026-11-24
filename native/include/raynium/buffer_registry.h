#pragma once
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>

namespace raynium {
struct BufferEntry {
    uint64_t size{};
    uint32_t usage{};
    bool pendingInitial{};
    radray::IntrusivePtr<radray::GpuBuffer> device;
    radray::unique_ptr<radray::gpu::Buffer> upload;
    radray::vector<radray::byte> host;
    radray::byte* mapped{};
    ~BufferEntry();
    radray::RDGBufferHandle Import(radray::RDGBuilder&) const;
    bool Contains(uint64_t offset,uint64_t length) const { return length && offset<=size && length<=size-offset; }
};
class BufferRegistry final {
public:
    bool Create(radray::gpu::Device&,const rn_buffer_desc&,rn_buffer_result&);
    bool Destroy(uint32_t id);
    void Advance(uint64_t submitted,uint64_t completed);
    radray::shared_ptr<BufferEntry> Find(uint32_t id) const;
    bool Flush(std::span<const rn_mapped_range>);
    void AddInitialUploads(radray::RDGBuilder&);
private:
    struct Retired { radray::shared_ptr<BufferEntry> entry; uint64_t sequence; };
    radray::unordered_map<uint64_t,radray::vector<Retired>> _pool;
    uint64_t _poolBytes{},_submitted{},_completed{};
    uint64_t _nextExpiry{UINT64_MAX};
    radray::shared_ptr<BufferEntry> Take(uint32_t uses,uint64_t size);
    struct Initial { radray::shared_ptr<BufferEntry> entry; radray::vector<radray::byte> bytes; };
    uint32_t _next{1};
    radray::unordered_map<uint32_t,radray::shared_ptr<BufferEntry>> _buffers;
    radray::vector<Initial> _initial;
};
}
