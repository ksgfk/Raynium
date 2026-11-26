#pragma once
#include <raynium/bridge.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/types.h>
#include <radray/runtime/rdg.h>
#include <atomic>
#include <mutex>

namespace raynium {
template<class T> struct ResourceHandle {
    rn_renderer* owner{};
    bool live{};
    T* next{};
    T** link{};
};
template<class T> class HandleList final {
public:
    ~HandleList() { while (_head) delete Remove(_head); }
    T* Insert(rn_renderer* owner, radray::unique_ptr<T> entry) {
        auto* value=entry.release();
        value->owner=owner;
        value->live=true;
        value->next=_head;
        value->link=&_head;
        if(_head) _head->link=&value->next;
        return _head=value;
    }
    T* Remove(T* value) {
        *value->link=value->next;
        if(value->next) value->next->link=value->link;
        value->live=false;
        return value;
    }
private:
    T* _head{};
};
template<class T> T* ResolveHandle(rn_renderer* owner,T* value) {
    return value && value->owner==owner && value->live ? value : nullptr;
}
struct HostReadStorage final : radray::ThreadSafeRefCountedBase<HostReadStorage> {
    explicit HostReadStorage(size_t size):bytes(size) {}
    std::atomic<bool> alive{true};
    radray::vector<radray::byte> bytes;
};
struct QuerySlot { uint64_t sequence{},ticks{}; bool available{}; };
struct QueryPoolStorage final : radray::ThreadSafeRefCountedBase<QueryPoolStorage> {
    explicit QueryPoolStorage(uint32_t count):slots(count) {}
    std::mutex mutex;
    radray::vector<QuerySlot> slots;
};
}

struct rn_query_pool : raynium::ResourceHandle<rn_query_pool> {
    radray::IntrusivePtr<raynium::QueryPoolStorage> storage;
};
struct rn_texture : raynium::ResourceHandle<rn_texture> {
    rn_texture_desc desc{};
    radray::IntrusivePtr<radray::GpuTexture> texture;
};
struct rn_buffer : raynium::ResourceHandle<rn_buffer> {
    uint32_t usage{};
    uint64_t size{};
    radray::IntrusivePtr<radray::GpuBuffer> gpu;
    radray::IntrusivePtr<raynium::HostReadStorage> readbackHost;
    radray::vector<radray::byte> shadowHost;
    radray::byte* mapped{};
    bool IsHostVisible() const noexcept { return gpu && gpu->IsHostVisible(); }
    bool IsReadback() const { return (usage & RN_BUFFER_MAP_READ)!=0; }
    bool Contains(uint64_t offset,uint64_t length) const { return length && offset<=size && length<=size-offset; }
    radray::RDGBufferHandle Import(radray::RDGBuilder&) const;
};
