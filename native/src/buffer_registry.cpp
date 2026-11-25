#include <raynium/buffer_registry.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
BufferEntry::~BufferEntry() { if(upload && mapped) upload->Unmap(); }
radray::RDGBufferHandle BufferEntry::Import(radray::RDGBuilder& graph) const {
    if(device) return graph.ImportPersistentOwned(device);
    return graph.ImportBuffer(upload.get(),BufferState::HostWrite,BufferState::HostWrite);
}
bool BufferRegistry::Valid(const rn_buffer_desc& desc,uint64_t maxSize) {
    const uint32_t uses=desc.usage & ~RN_BUFFER_CLIENT_STORAGE;
    return desc.struct_size==sizeof(desc) && uses && !(uses&~2047u) && desc.size &&
           (!(uses&RN_BUFFER_FENCE_ROTATED) || ((uses&RN_BUFFER_MAP_WRITE) && !(uses&RN_BUFFER_COPY_DST))) &&
           desc.size<=maxSize && desc.size<=PTRDIFF_MAX &&
           (!(uses&RN_BUFFER_MAP_READ) || !(uses&~(RN_BUFFER_MAP_READ|RN_BUFFER_COPY_DST)));
}
radray::shared_ptr<BufferEntry> BufferRegistry::Allocate(Device& gpu,uint32_t uses,uint64_t size) {
    auto entry=radray::make_shared<BufferEntry>();
    entry->size=size;
    entry->usage=uses;
    BufferUses gpuUses{};
    if(uses&RN_BUFFER_COPY_SRC) gpuUses|=BufferUse::CopySource;
    if(uses&RN_BUFFER_COPY_DST) gpuUses|=BufferUse::CopyDestination;
    if(uses&RN_BUFFER_VERTEX) gpuUses|=BufferUse::Vertex;
    if(uses&RN_BUFFER_INDEX) gpuUses|=BufferUse::Index;
    if(uses&RN_BUFFER_UNIFORM) gpuUses|=BufferUse::CBuffer;
    if(uses&RN_BUFFER_TEXEL) gpuUses|=BufferUse::Resource;
    if(uses&RN_BUFFER_INDIRECT) gpuUses|=BufferUse::Indirect|BufferUse::CopySource;
    if(uses&RN_BUFFER_MAP_READ) return entry;
    if(uses&RN_BUFFER_FENCE_ROTATED) {
        entry->upload=gpu.CreateBuffer({.Size=size,.Memory=MemoryType::Upload,.Usage=gpuUses|BufferUse::MapWrite,
            .Hints=ResourceHint::PersistentMap}).Release();
        if(!entry->upload) return {};
        entry->mapped=static_cast<radray::byte*>(entry->upload->Map(0,size));
        if(!entry->mapped) return {};
    } else {
        entry->device=radray::GpuBuffer::Create(gpu,{size,gpuUses|BufferUse::CopyDestination}).Release();
        if(!entry->device) return {};
    }
    return entry;
}
void BufferRegistry::Register(uint32_t id,radray::shared_ptr<BufferEntry> entry,radray::vector<radray::byte> initial,bool flush) {
    entry->id=id;
    if(flush) entry->upload->FlushMappedRange({0,entry->size});
    if(!initial.empty()) {
        entry->pendingInitial=true;
        _initial.push_back({entry,std::move(initial)});
    }
    _buffers.emplace(id,std::move(entry));
}
namespace {
constexpr uint64_t kMaxPooledBuffer=16u<<20;
constexpr uint64_t kPoolBudget=64u<<20;
constexpr uint64_t kIdleSubmissions=120;
uint64_t PoolKey(uint32_t usage,uint64_t size) { return (uint64_t(usage & ~(RN_BUFFER_COPY_DST | RN_BUFFER_MAP_WRITE))<<32)|size; }
}

bool BufferRegistry::Prewarm(Device& gpu) {
    constexpr uint64_t size=512u<<10;
    for (uint32_t binding : {RN_BUFFER_UNIFORM, RN_BUFFER_COPY_SRC}) {
        const uint32_t usage=RN_BUFFER_FENCE_ROTATED|RN_BUFFER_MAP_WRITE|binding;
        auto entry=Allocate(gpu,usage,size);
        if(!entry) return false;
        _pool[PoolKey(usage,size)].push_back({std::move(entry),0});
        _poolBytes+=size;
    }
    _nextExpiry=kIdleSubmissions;
    return true;
}

radray::shared_ptr<BufferEntry> BufferRegistry::Take(uint32_t uses,uint64_t size,uint64_t completed) {
    if(size>kMaxPooledBuffer || (uses&RN_BUFFER_MAP_READ)) return {};
    std::lock_guard lock(_poolMutex);
    auto found=_pool.find(PoolKey(uses,size));
    if(found==_pool.end()) return {};
    auto& bucket=found->second;
    for(size_t i=bucket.size();i>0;--i) {
        auto& retired=bucket[i-1];
        if(retired.sequence>completed || retired.entry.use_count()!=1) continue;
        auto entry=std::move(retired.entry);
        if(i!=bucket.size()) retired=std::move(bucket.back());
        bucket.pop_back();
        if(bucket.empty()) _pool.erase(found);
        entry->usage=uses;
        _poolBytes-=size;
        return entry;
    }
    return {};
}
bool BufferRegistry::Destroy(uint32_t id) {
    auto it=_buffers.find(id);
    if(it==_buffers.end()) return false;
    auto entry=std::move(it->second);
    _buffers.erase(it);
    std::lock_guard lock(_poolMutex);
    if((entry->usage&RN_BUFFER_MAP_READ) || entry->pendingInitial || entry->size>kMaxPooledBuffer ||
       entry->size>kPoolBudget-_poolBytes) return true;
    const auto key=PoolKey(entry->usage,entry->size);
    _poolBytes+=entry->size;
    _pool[key].push_back({std::move(entry),_submitted});
    _nextExpiry=std::min(_nextExpiry,_submitted+kIdleSubmissions);
    return true;
}
void BufferRegistry::Advance(uint64_t submitted,uint64_t completed) {
    radray::vector<radray::shared_ptr<BufferEntry>> expired;
    std::lock_guard lock(_poolMutex);
    _submitted=submitted;
    if(submitted<_nextExpiry) return;
    _nextExpiry=UINT64_MAX;
    for(auto it=_pool.begin();it!=_pool.end();) {
        std::erase_if(it->second,[&](Retired& item) {
            const auto expiry=item.sequence+kIdleSubmissions;
            if(expiry<=submitted) { _poolBytes-=item.entry->size; expired.push_back(std::move(item.entry)); return true; }
            _nextExpiry=std::min(_nextExpiry,expiry);
            return false;
        });
        if(it->second.empty()) it=_pool.erase(it);
        else ++it;
    }
}
radray::shared_ptr<BufferEntry> BufferRegistry::Find(uint32_t id) const {
    auto found=_buffers.find(id);
    return found==_buffers.end()?nullptr:found->second;
}
bool BufferRegistry::Flush(std::span<const rn_mapped_range> ranges) {
    for(const auto& range:ranges) {
        auto buffer=Find(range.id);
        if(!buffer || range.reserved || !buffer->Contains(range.offset,range.length)) return false;
        if(buffer->upload) buffer->upload->FlushMappedRange({range.offset,range.length});
        else if(!(buffer->usage & RN_BUFFER_MAP_WRITE) || (buffer->usage & RN_BUFFER_COPY_DST)) return false;
    }
    return true;
}
bool BufferRegistry::UploadShadow(uint32_t id, uint64_t offset, radray::vector<radray::byte> bytes) {
    auto entry = Find(id);
    if (!entry || !entry->device || !entry->Contains(offset, bytes.size())) return false;
    _initial.push_back({std::move(entry), std::move(bytes), offset});
    return true;
}
void BufferRegistry::AddInitialUploads(radray::RDGBuilder& graph) {
    for(auto& initial:_initial) {
        auto target=graph.ImportPersistentOwned(initial.entry->device);
        initial.entry->pendingInitial=false;
        graph.UploadBuffer(target,initial.offset,radray::RDGUploadSource::OwnedBytes(std::move(initial.bytes)));
        graph.MarkOutput(target);
    }
    _initial.clear();
}
}
