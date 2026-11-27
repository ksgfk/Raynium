#include <raynium/frame_readbacks.h>

#include <cstring>
#include <utility>

namespace raynium {
MappedReadback::MappedReadback(radray::unique_ptr<radray::gpu::Buffer> resource, const radray::byte* data)
    : Buffer(std::move(resource)), Mapped(data) {}

void ApplyHostBufferWrite(const HostBufferWrite& write) {
    if (!write.Host->Alive.load()) return;
    auto* dest = write.Host->Bytes.data() + write.Destination;
    if (!write.Bytes.empty()) {
        std::memcpy(dest, write.Bytes.data(), write.Bytes.size());
        return;
    }
    const auto* source = write.Staging->Mapped + write.Offset;
    for (uint32_t row = 0; row < write.Rows; ++row) {
        std::memcpy(dest + row * write.RowBytes, source + row * write.Pitch, write.RowBytes);
    }
}

void FrameReadbacks::AddInline(rn_buffer* target, uint64_t destination, radray::vector<radray::byte>&& bytes) {
    _entries.push_back({target->ReadbackHost, destination, bytes.size(), 1, bytes.size(), 0, std::move(bytes), {}});
}

bool FrameReadbacks::Reserve(
    rn_buffer* target,
    uint64_t destination,
    uint64_t rowBytes,
    uint32_t rows,
    uint64_t pitch,
    uint64_t alignment,
    uint64_t& offset) {
    if (!alignment || !rows || !rowBytes || pitch < rowBytes || pitch > UINT64_MAX / rows) return false;
    const uint64_t padding = (alignment - _size % alignment) % alignment;
    if (padding > UINT64_MAX - _size) return false;
    offset = _size + padding;
    if (pitch * rows > UINT64_MAX - offset || !BufferContainsRange(*target, destination, rowBytes * rows)) return false;
    _size = offset + pitch * rows;
    _entries.push_back({target->ReadbackHost, destination, rowBytes, rows, pitch, offset, {}, {}});
    return true;
}

bool FrameReadbacks::Prepare(radray::gpu::Device& device) {
    if (!_size || (_storage && _storage->UseCount() == 1 && _storage->Buffer->GetDesc().Size >= _size)) return true;
    auto buffer = device.CreateBuffer({
                                          .Size = _size,
                                          .Memory = radray::gpu::MemoryType::HostReadBack,
                                          .Usage = radray::gpu::BufferUse::CopyDestination | radray::gpu::BufferUse::MapRead,
                                      })
                      .Release();
    if (!buffer) return false;
    auto* mapped = static_cast<const radray::byte*>(buffer->GetMappedData().Get());
    if (!mapped) return false;
    _storage = radray::IntrusivePtr<MappedReadback>(new MappedReadback(std::move(buffer), mapped));
    _state = radray::gpu::BufferState::Undefined;
    return true;
}

radray::RDGBufferHandle FrameReadbacks::Import(radray::RDGBuilder& graph) const {
    return graph.ImportBuffer(_storage->Buffer.get(), _state, radray::gpu::BufferState::HostRead);
}

void FrameReadbacks::Submitted(uint64_t sequence) {
    if (!_entries.empty()) _sequence = sequence;
    if (_size) _state = radray::gpu::BufferState::HostRead;
}

uint64_t FrameReadbacks::Sequence() const noexcept {
    return _sequence;
}

bool FrameReadbacks::Collect(uint64_t completed, radray::vector<HostBufferWrite>& output) {
    if (!_sequence || _sequence > completed) return true;
    if (_size && !_storage->Buffer->InvalidateMappedRange({0, _size})) return false;
    for (auto& entry : _entries) {
        if (entry.Bytes.empty()) entry.Staging = _storage;
        output.push_back(std::move(entry));
    }
    _entries.clear();
    _size = 0;
    _sequence = 0;
    return true;
}
}  // namespace raynium
