#pragma once
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <mutex>

namespace raynium {
struct BufferEntry {
    uint64_t size{};
    uint32_t id{}, usage{};
    bool pendingInitial{};
    radray::IntrusivePtr<radray::GpuBuffer> device;
    radray::unique_ptr<radray::gpu::Buffer> upload;
    bool IsReadback() const { return (usage & RN_BUFFER_MAP_READ) != 0; }
    radray::byte* mapped{};
    ~BufferEntry();
    radray::RDGBufferHandle Import(radray::RDGBuilder&) const;
    bool Contains(uint64_t offset,uint64_t length) const { return length && offset<=size && length<=size-offset; }
};
class BufferRegistry final {
public:
    bool Prewarm(radray::gpu::Device&);
    static bool Valid(const rn_buffer_desc&, uint64_t maxSize);
    static radray::shared_ptr<BufferEntry> Allocate(radray::gpu::Device&, uint32_t uses, uint64_t size);
    radray::shared_ptr<BufferEntry> Take(uint32_t uses,uint64_t size,uint64_t completed);
    void Register(uint32_t id,radray::shared_ptr<BufferEntry> entry,radray::vector<radray::byte> initial,bool flush);
    bool Destroy(uint32_t id);
    void Advance(uint64_t submitted,uint64_t completed);
    radray::shared_ptr<BufferEntry> Find(uint32_t id) const;
    bool Flush(std::span<const rn_mapped_range>);
    bool UploadShadow(uint32_t id, uint64_t offset, radray::vector<radray::byte> bytes);
    void AddInitialUploads(radray::RDGBuilder&);
private:
    struct Retired { radray::shared_ptr<BufferEntry> entry; uint64_t sequence; };
    radray::unordered_map<uint64_t,radray::vector<Retired>> _pool;
    std::mutex _poolMutex;
    uint64_t _poolBytes{},_submitted{};
    uint64_t _nextExpiry{UINT64_MAX};
    struct Initial { radray::shared_ptr<BufferEntry> entry; radray::vector<radray::byte> bytes; uint64_t offset{}; };
    radray::unordered_map<uint32_t,radray::shared_ptr<BufferEntry>> _buffers;
    radray::vector<Initial> _initial;
};
}
