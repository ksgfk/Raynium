#pragma once

#include <raynium/buffer_registry.h>

namespace raynium {
struct HostBufferWrite {
    uint32_t id;
    uint64_t offset;
    radray::vector<radray::byte> bytes;
};
class FrameReadbacks final {
public:
    void AddInline(const radray::shared_ptr<BufferEntry>&,uint64_t destination,radray::vector<radray::byte>&&);
    bool Reserve(const radray::shared_ptr<BufferEntry>&,uint64_t destination,uint64_t rowBytes,uint32_t rows,
                 uint64_t pitch,uint64_t alignment,uint64_t& offset);
    bool Prepare(radray::gpu::Device&);
    radray::RDGBufferHandle Import(radray::RDGBuilder&) const;
    void Submitted(uint64_t sequence);
    uint64_t Sequence() const { return _sequence; }
    void Collect(uint64_t completed, radray::vector<HostBufferWrite>& output);
private:
    struct Publication {
        radray::shared_ptr<BufferEntry> target;
        uint64_t destination,rowBytes;
        uint32_t rows;
        uint64_t pitch,offset;
        radray::vector<radray::byte> bytes;
    };
    struct Storage {
        radray::unique_ptr<radray::gpu::Buffer> buffer;
        const radray::byte* mapped;
        ~Storage() { buffer->Unmap(); }
    };
    radray::unique_ptr<Storage> _storage;
    radray::vector<Publication> _entries;
    uint64_t _size{},_sequence{};
    radray::gpu::BufferStates _state{radray::gpu::BufferState::Undefined};
};
}
