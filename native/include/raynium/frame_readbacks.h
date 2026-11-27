#pragma once

#include <cstdint>

#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/buffer_pool.h>

namespace raynium {
class MappedReadback final : public radray::ThreadSafeRefCountedBase<MappedReadback> {
public:
    MappedReadback(radray::unique_ptr<radray::gpu::Buffer> resource, const radray::byte* data);

    radray::unique_ptr<radray::gpu::Buffer> Buffer;
    const radray::byte* Mapped{};
};

struct HostBufferWrite {
    radray::IntrusivePtr<HostReadStorage> Host;
    uint64_t Destination{};
    uint64_t RowBytes{};
    uint32_t Rows{};
    uint64_t Pitch{};
    uint64_t Offset{};
    radray::vector<radray::byte> Bytes;
    radray::IntrusivePtr<MappedReadback> Staging;
};

void ApplyHostBufferWrite(const HostBufferWrite& write);

class FrameReadbacks final {
public:
    void AddInline(rn_buffer* target, uint64_t destination, radray::vector<radray::byte>&& bytes);
    bool Reserve(
        rn_buffer* target,
        uint64_t destination,
        uint64_t rowBytes,
        uint32_t rows,
        uint64_t pitch,
        uint64_t alignment,
        uint64_t& offset);
    bool Prepare(radray::gpu::Device& device);
    radray::RDGBufferHandle Import(radray::RDGBuilder& graph) const;
    void Submitted(uint64_t sequence);
    uint64_t Sequence() const noexcept;
    bool Collect(uint64_t completed, radray::vector<HostBufferWrite>& output);

private:
    radray::IntrusivePtr<MappedReadback> _storage;
    radray::vector<HostBufferWrite> _entries;
    uint64_t _size{};
    uint64_t _sequence{};
    radray::gpu::BufferStates _state{radray::gpu::BufferState::Undefined};
};
}  // namespace raynium
