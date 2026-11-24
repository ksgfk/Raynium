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
bool BufferRegistry::Create(Device& gpu,const rn_buffer_desc& desc,rn_buffer_result& result) {
    const uint32_t uses=desc.usage & ~RN_BUFFER_CLIENT_STORAGE;
    if(desc.struct_size!=sizeof(desc) || !uses || (uses&~1023u) || !_next || !desc.size ||
       desc.size>gpu.GetCapabilities().Limits.MaxBufferSize || desc.size>PTRDIFF_MAX ||
       ((uses&RN_BUFFER_MAP_READ) && (uses&~(RN_BUFFER_MAP_READ|RN_BUFFER_COPY_DST)))) return false;
    auto entry=Take(uses,desc.size);
    if(!entry) {
        entry=radray::make_shared<BufferEntry>();
        entry->size=desc.size;
        entry->usage=uses;
        BufferUses gpuUses{};
        if(uses&RN_BUFFER_COPY_SRC) gpuUses|=BufferUse::CopySource;
        if(uses&RN_BUFFER_COPY_DST) gpuUses|=BufferUse::CopyDestination;
        if(uses&RN_BUFFER_VERTEX) gpuUses|=BufferUse::Vertex;
        if(uses&RN_BUFFER_INDEX) gpuUses|=BufferUse::Index;
        if(uses&RN_BUFFER_UNIFORM) gpuUses|=BufferUse::CBuffer;
        if(uses&RN_BUFFER_TEXEL) gpuUses|=BufferUse::Resource;
        if(uses&RN_BUFFER_INDIRECT) gpuUses|=BufferUse::Indirect|BufferUse::CopySource;
        if(uses&RN_BUFFER_MAP_READ) {
            entry->host.resize(desc.size);
            entry->mapped=entry->host.data();
        } else if((uses&RN_BUFFER_MAP_WRITE) && !(uses&RN_BUFFER_COPY_DST)) {
            entry->upload=gpu.CreateBuffer({.Size=desc.size,.Memory=MemoryType::Upload,.Usage=gpuUses|BufferUse::MapWrite,
                .Hints=ResourceHint::PersistentMap}).Release();
            if(!entry->upload) return false;
            entry->mapped=static_cast<radray::byte*>(entry->upload->Map(0,desc.size));
            if(!entry->mapped) return false;
        } else {
            entry->device=radray::GpuBuffer::Create(gpu,{desc.size,gpuUses|BufferUse::CopyDestination}).Release();
            if(!entry->device) return false;
        }
    }
    if(desc.initial) {
        if(entry->mapped) {
            std::memcpy(entry->mapped,desc.initial,desc.size);
            if(entry->upload) entry->upload->FlushMappedRange({0,desc.size});
        } else {
            auto* first=static_cast<const radray::byte*>(desc.initial);
            entry->pendingInitial=true;
            _initial.push_back({entry,{first,first+desc.size}});
        }
    }
    auto id=_next++;
    result={id,0,entry->mapped};
    _buffers.emplace(id,std::move(entry));
    return true;
}
namespace {
constexpr uint64_t kMaxPooledBuffer=16u<<20;
constexpr uint64_t kPoolBudget=64u<<20;
constexpr uint64_t kIdleSubmissions=120;
uint64_t PoolKey(uint32_t usage,uint64_t size) { return (uint64_t(usage)<<32)|size; }
}

radray::shared_ptr<BufferEntry> BufferRegistry::Take(uint32_t uses,uint64_t size) {
    if(size>kMaxPooledBuffer || (uses&RN_BUFFER_MAP_READ)) return {};
    auto found=_pool.find(PoolKey(uses,size));
    if(found==_pool.end()) return {};
    auto& bucket=found->second;
    for(size_t i=bucket.size();i>0;--i) {
        auto& retired=bucket[i-1];
        if(retired.sequence>_completed || retired.entry.use_count()!=1) continue;
        auto entry=std::move(retired.entry);
        if(i!=bucket.size()) retired=std::move(bucket.back());
        bucket.pop_back();
        if(bucket.empty()) _pool.erase(found);
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
    if((entry->usage&RN_BUFFER_MAP_READ) || entry->pendingInitial || entry->size>kMaxPooledBuffer ||
       entry->size>kPoolBudget-_poolBytes) return true;
    const auto key=PoolKey(entry->usage,entry->size);
    _poolBytes+=entry->size;
    _pool[key].push_back({std::move(entry),_submitted});
    _nextExpiry=std::min(_nextExpiry,_submitted+kIdleSubmissions);
    return true;
}
void BufferRegistry::Advance(uint64_t submitted,uint64_t completed) {
    _submitted=submitted;
    _completed=completed;
    if(submitted<_nextExpiry) return;
    _nextExpiry=UINT64_MAX;
    for(auto it=_pool.begin();it!=_pool.end();) {
        std::erase_if(it->second,[&](const Retired& item) {
            const auto expiry=item.sequence+kIdleSubmissions;
            if(expiry<=submitted) { _poolBytes-=item.entry->size; return true; }
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
        if(!buffer || !buffer->upload || range.reserved || !buffer->Contains(range.offset,range.length)) return false;
        buffer->upload->FlushMappedRange({range.offset,range.length});
    }
    return true;
}
void BufferRegistry::AddInitialUploads(radray::RDGBuilder& graph) {
    for(auto& initial:_initial) {
        auto target=graph.ImportPersistentOwned(initial.entry->device);
        initial.entry->pendingInitial=false;
        graph.UploadBuffer(target,0,radray::RDGUploadSource::OwnedBytes(std::move(initial.bytes)));
        graph.MarkOutput(target);
    }
    _initial.clear();
}
}
