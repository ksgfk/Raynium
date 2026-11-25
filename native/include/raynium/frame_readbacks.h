#pragma once

#include <raynium/buffer_pool.h>

namespace raynium {
struct MappedReadback final : radray::ThreadSafeRefCountedBase<MappedReadback> {
    MappedReadback(radray::unique_ptr<radray::gpu::Buffer> resource,const radray::byte* data)
        :buffer(std::move(resource)),mapped(data) {}
    radray::unique_ptr<radray::gpu::Buffer> buffer;
    const radray::byte* mapped;
};
struct HostBufferWrite {
    radray::IntrusivePtr<HostReadStorage> host;
    uint64_t destination,rowBytes;
    uint32_t rows;
    uint64_t pitch,offset;
    radray::vector<radray::byte> bytes;
    radray::IntrusivePtr<MappedReadback> staging;
    void Apply() const;
};
class FrameReadbacks final {
public:
    void AddInline(rn_buffer*,uint64_t destination,radray::vector<radray::byte>&&);
    bool Reserve(rn_buffer*,uint64_t destination,uint64_t rowBytes,uint32_t rows,
                 uint64_t pitch,uint64_t alignment,uint64_t& offset);
    bool Prepare(radray::gpu::Device&);
    radray::RDGBufferHandle Import(radray::RDGBuilder&) const;
    void Submitted(uint64_t sequence);
    uint64_t Sequence() const { return _sequence; }
    bool Collect(uint64_t completed, radray::vector<HostBufferWrite>& output);
private:
    radray::IntrusivePtr<MappedReadback> _storage;
    radray::vector<HostBufferWrite> _entries;
    uint64_t _size{},_sequence{};
    radray::gpu::BufferStates _state{radray::gpu::BufferState::Undefined};
};
}
