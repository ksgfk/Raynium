#include <raynium/resource_handles.h>

namespace raynium {
HostReadStorage::HostReadStorage(size_t size) : Bytes(size) {}

QueryPoolStorage::QueryPoolStorage(uint32_t count) : Slots(count) {}

bool IsHostVisibleBuffer(const rn_buffer& buffer) noexcept {
    return buffer.Gpu && buffer.Gpu->IsHostVisible();
}

bool IsReadbackBuffer(const rn_buffer& buffer) noexcept {
    return (buffer.Usage & RN_BUFFER_MAP_READ) != 0;
}

bool BufferContainsRange(const rn_buffer& buffer, uint64_t offset, uint64_t length) noexcept {
    return length && offset <= buffer.Size && length <= buffer.Size - offset;
}

radray::RDGBufferHandle ImportBuffer(const rn_buffer& buffer, radray::RDGBuilder& graph) {
    return graph.ImportPersistentOwned(buffer.Gpu);
}
}  // namespace raynium
