#include <raynium/terrain_draw.h>
#include <raynium/mesh_pass.h>
#include <radray/profiler.h>
#include <algorithm>
#include <bit>
#include <cassert>
#include <limits>

namespace raynium {
namespace {
uint64_t Mix(uint64_t value) noexcept {
    value^=value>>30; value*=0xbf58476d1ce4e5b9ULL;
    value^=value>>27; value*=0x94d049bb133111ebULL; return value^(value>>31);
}
void Sort(std::vector<TerrainSortItem>& items,std::vector<TerrainSortItem>& scratch,bool depthFirst) {
    const auto key=[depthFirst](const TerrainSortItem& item) { return depthFirst?std::rotl(item.sortKey,32):item.sortKey; };
    if(items.size()<2) return;
    if(items.size()<32) {
        for(size_t i=1;i<items.size();++i) {
            const auto item=items[i]; size_t j=i;
            while(j && key(item)<key(items[j-1])) { items[j]=items[j-1]; --j; }
            items[j]=item;
        }
        return;
    }
    uint64_t differing=0; bool sorted=true;
    for(size_t i=1;i<items.size();++i) {
        differing|=items[i].sortKey^items[0].sortKey;
        sorted&=key(items[i-1])<=key(items[i]);
    }
    if(sorted) return;
    if(scratch.size()<items.size()) scratch.resize(items.size());
    auto* source=&items; auto* destination=&scratch;
    for(uint32_t pass=0;pass<8;++pass) {
        const uint32_t shift=((pass+(depthFirst?4:0))&7)*8;
        if(!((differing>>shift)&255)) continue;
        std::array<size_t,256> offsets{};
        for(size_t i=0;i<items.size();++i) ++offsets[((*source)[i].sortKey>>shift)&255];
        size_t next=0;
        for(auto& offset:offsets) { const auto count=offset; offset=next; next+=count; }
        for(size_t i=0;i<items.size();++i) {
            const auto& item=(*source)[i]; (*destination)[offsets[(item.sortKey>>shift)&255]++]=item;
        }
        std::swap(source,destination);
    }
    if(source!=&items) std::copy_n(source->data(),items.size(),items.data());
}
bool Same(const TerrainDrawBatch& batch,const TerrainBindingState& state) {
    const auto& v=state.vertices; const auto& i=state.indices;
    return batch.material->parameters.get()==state.material->parameters.get() &&
        batch.vertices.Target==v.Target && batch.vertices.Offset==v.Offset && batch.vertices.Size==v.Size &&
        batch.indices.Target==i.Target && batch.indices.Offset==i.Offset && batch.indices.Stride==i.Stride;
}
}
bool TerrainBindingStates::Key::operator==(const Key& other) const noexcept {
    return vertices.Target==other.vertices.Target && vertices.Offset==other.vertices.Offset && vertices.Size==other.vertices.Size &&
        indices.Target==other.indices.Target && indices.Offset==other.indices.Offset && indices.Stride==other.indices.Stride &&
        parameters==other.parameters && materialVersion==other.materialVersion;
}
size_t TerrainBindingStates::Hash::operator()(const Key& key) const noexcept {
    uint64_t hash=Mix(reinterpret_cast<uintptr_t>(key.vertices.Target));
    const auto add=[&](uint64_t value) { hash=Mix(hash^Mix(value)); };
    add(key.vertices.Offset); add(key.vertices.Size); add(reinterpret_cast<uintptr_t>(key.indices.Target));
    add(key.indices.Offset); add(key.indices.Stride); add(reinterpret_cast<uintptr_t>(key.parameters)); add(key.materialVersion);
    return size_t(hash);
}
bool TerrainBindingStates::Acquire(const TerrainDrawTemplate& geometry,const MeshMaterialBinding& material,uint32_t& id,radray::string& error) {
    const Key key{geometry.vertices,geometry.indices,material.parameters.get(),material.sortKey};
    if(const auto found=_lookup.find(key);found!=_lookup.end()) {
        auto& entry=_entries[found->second];
        if(entry.references==UINT32_MAX) { error="Terrain binding reference count exhausted"; return false; }
        ++entry.references; id=found->second; return true;
    }
    if(!_freeHead && _states.size()>_limit) { error="Terrain binding state IDs exhausted"; return false; }
    const auto next=_freeHead?_freeHead:uint32_t(_states.size());
    if(!_freeHead && _states.size()==_states.capacity()) {
        const auto capacity=std::min<uint64_t>(uint64_t(_limit)+1,std::max<uint64_t>(16,uint64_t(_states.capacity())*2));
        _states.reserve(size_t(capacity)); _entries.reserve(size_t(capacity));
    } else if(!_freeHead && _entries.size()==_entries.capacity()) _entries.reserve(_states.capacity());
    _lookup.emplace(key,next);
    if(_freeHead) _freeHead=_entries[next].nextFree;
    else { _states.emplace_back(); _entries.emplace_back(); }
    _states[next]={geometry.vertices,geometry.indices,&material}; _entries[next]={key,1,0}; id=next; return true;
}
void TerrainBindingStates::Release(uint32_t id) noexcept {
    if(!id) return;
    assert(id<_entries.size() && _entries[id].references);
    auto& entry=_entries[id];
    if(--entry.references) return;
    _lookup.erase(entry.key); _states[id]={}; entry.nextFree=_freeHead; _freeHead=id;
}
uint32_t TerrainIndirectChunk(uint32_t remaining,uint32_t nativeLimit) noexcept { return std::min(remaining,nativeLimit); }
bool SelectTerrainSubmission(uint32_t flags,const radray::render::RenderDeviceCapabilities& caps,bool& indirect,radray::string& reason) {
    indirect=false; reason.clear();
    if(flags>RN_RENDERER_INDIRECT) { reason="Unknown or mutually exclusive terrain submission flags"; return false; }
    if(flags==RN_RENDERER_DIRECT) return true;
    reason=!caps.Features.IndirectDraw?"indirect draws unsupported":
        !caps.Features.IndirectFirstInstance?"nonzero indirect firstInstance unsupported":
        caps.Limits.MaxDrawIndirectCount<=1?"native multi-record indirect unsupported":"";
    indirect=reason.empty();
    return flags!=RN_RENDERER_INDIRECT || indirect;
}
bool BuildTerrainDraws(TerrainFrameLists& lists,std::span<const TerrainBindingState> states,bool oit,radray::string& error) {
    lists.arguments.clear(); lists.batches.clear(); lists.layers={};
    for(uint32_t layer=0;layer<3;++layer) {
        const bool depthFirst=layer==RN_MATERIAL_TRANSLUCENT && !oit;
        const auto& items=lists.visible[layer]; auto& order=lists.order[layer];
        if(items.size()!=order.size()) { error="Terrain payload and sort item counts differ"; return false; }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Sort");
            Sort(order,lists.sortScratch,depthFirst);
        }
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.BuildArguments");
        auto& result=lists.layers[layer];
        result.firstBatch=uint32_t(lists.batches.size()); result.firstRecord=uint32_t(lists.arguments.size());
        for(const auto& sorted:order) {
            if(sorted.payloadIndex>=items.size()) { error="Invalid terrain payload index"; return false; }
            const auto& item=items[sorted.payloadIndex];
            if(!item.stateId || item.stateId>=states.size() || !states[item.stateId].material || !item.ranges) {
                error="Invalid terrain binding state or ranges"; return false;
            }
            const auto& state=states[item.stateId];
            auto mask=item.mask;
            result.selectedGroups+=std::popcount(mask);
            if(!mask) continue;
            if(lists.batches.size()==result.firstBatch || !Same(lists.batches.back(),state))
                lists.batches.push_back({state.vertices,state.indices,state.material,uint32_t(lists.arguments.size()),0});
            while(mask) {
                auto bit=uint32_t(std::countr_zero(mask)); mask&=mask-1;
                const auto& range=item.ranges[bit];
                auto count=range.count;
                if(!depthFirst && item.instanceCount==1) {
                    while(bit<31 && (item.mergeMask&(1u<<bit)) && (mask&(1u<<(bit+1))) &&
                          uint64_t(count)+item.ranges[bit+1].count<=std::numeric_limits<uint32_t>::max()) {
                        count+=item.ranges[++bit].count; mask&=~(1u<<bit);
                    }
                }
                const uint64_t indices=uint64_t(count)*item.instanceCount;
                if(indices>std::numeric_limits<uint64_t>::max()-result.indicesInstances ||
                   lists.arguments.size()>=std::numeric_limits<uint32_t>::max()) {
                    error="Terrain draw record or submitted index count overflow"; return false;
                }
                result.indicesInstances+=indices;
                lists.arguments.push_back({count,item.instanceCount,range.first,range.vertex,item.firstInstance});
                ++lists.batches.back().count;
            }
        }
        result.batchCount=uint32_t(lists.batches.size())-result.firstBatch;
        result.recordCount=uint32_t(lists.arguments.size())-result.firstRecord;
    }
    uint64_t submitted=0;
    for(uint32_t layer=0;layer<3;++layer) {
        const uint64_t multiplier=oit && layer==2?3:1,indices=lists.layers[layer].indicesInstances;
        if(indices>(std::numeric_limits<uint64_t>::max()-submitted)/multiplier) {
            error="Terrain submitted index count overflow"; return false;
        }
        submitted+=indices*multiplier;
    }
    return true;
}
}
