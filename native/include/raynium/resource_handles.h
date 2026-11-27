#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/bridge.h>

namespace raynium {
template <class T>
struct ResourceHandle {
    rn_renderer* Owner{};
    bool Live{};
    T* Next{};
    T** Link{};
};

template <class T>
class HandleList final {
public:
    ~HandleList() {
        while (_head) delete Remove(_head);
    }

    T* Insert(rn_renderer* owner, radray::unique_ptr<T> entry) {
        auto* value = entry.release();
        value->Owner = owner;
        value->Live = true;
        value->Next = _head;
        value->Link = &_head;
        if (_head) _head->Link = &value->Next;
        return _head = value;
    }

    T* Remove(T* value) {
        *value->Link = value->Next;
        if (value->Next) value->Next->Link = value->Link;
        value->Live = false;
        return value;
    }

private:
    T* _head{};
};

template <class T>
T* ResolveHandle(rn_renderer* owner, T* value) {
    return value && value->Owner == owner && value->Live ? value : nullptr;
}

class HostReadStorage final : public radray::ThreadSafeRefCountedBase<HostReadStorage> {
public:
    explicit HostReadStorage(size_t size);

    std::atomic<bool> Alive{true};
    radray::vector<radray::byte> Bytes;
};

struct QuerySlot {
    uint64_t Sequence{};
    uint64_t Ticks{};
    bool Available{};
};

class QueryPoolStorage final : public radray::ThreadSafeRefCountedBase<QueryPoolStorage> {
public:
    explicit QueryPoolStorage(uint32_t count);

    std::mutex Mutex;
    radray::vector<QuerySlot> Slots;
};
}  // namespace raynium

struct rn_query_pool : raynium::ResourceHandle<rn_query_pool> {
    radray::IntrusivePtr<raynium::QueryPoolStorage> Storage;
};

struct rn_texture : raynium::ResourceHandle<rn_texture> {
    rn_texture_desc Desc{};
    radray::IntrusivePtr<radray::GpuTexture> Texture;
};

struct rn_buffer : raynium::ResourceHandle<rn_buffer> {
    uint32_t Usage{};
    uint64_t Size{};
    radray::IntrusivePtr<radray::GpuBuffer> Gpu;
    radray::IntrusivePtr<raynium::HostReadStorage> ReadbackHost;
    radray::vector<radray::byte> ShadowHost;
    radray::byte* Mapped{};
};

namespace raynium {
bool IsHostVisibleBuffer(const rn_buffer& buffer) noexcept;
bool IsReadbackBuffer(const rn_buffer& buffer) noexcept;
bool BufferContainsRange(const rn_buffer& buffer, uint64_t offset, uint64_t length) noexcept;
radray::RDGBufferHandle ImportBuffer(const rn_buffer& buffer, radray::RDGBuilder& graph);
}  // namespace raynium
