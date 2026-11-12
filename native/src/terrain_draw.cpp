#include <raynium/terrain_draw.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <Eigen/LU>
#include <cstring>
#include <cmath>
#include <raynium/mesh_pass.h>
#include <radray/profiler.h>
#include <algorithm>
#include <bit>
#include <cassert>
#include <limits>
#include <string_view>
#include <span>
#include <utility>

namespace raynium {
namespace {
uint64_t Mix(uint64_t value) noexcept {
    value^=value>>30; value*=0xbf58476d1ce4e5b9ULL;
    value^=value>>27; value*=0x94d049bb133111ebULL; return value^(value>>31);
}
void Sort(radray::vector<TerrainSortItem>& items,radray::vector<TerrainSortItem>& scratch,bool depthFirst) {
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
        radray::array<size_t,256> offsets{};
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
bool TerrainBindingStates::Acquire(const TerrainDrawTemplate& geometry,const MeshMaterialBinding& material,uint32_t& id) {
    if(_states.size()!=_entries.size() || (_freeHead && (_freeHead>=_entries.size() || _entries[_freeHead].references)))
        RADRAY_ABORT("Invalid terrain binding state storage");
    const Key key{geometry.vertices,geometry.indices,material.parameters.get(),material.sortKey};
    if(const auto found=_lookup.find(key);found!=_lookup.end()) {
        if(found->second>=_entries.size() || !_entries[found->second].references)
            RADRAY_ABORT("Invalid terrain binding lookup: {}",found->second);
        auto& entry=_entries[found->second];
        if(entry.references==UINT32_MAX) { RADRAY_ERR_LOG("Terrain binding reference count exhausted"); return false; }
        ++entry.references; id=found->second; return true;
    }
    if(!_freeHead && _states.size()>_limit) { RADRAY_ERR_LOG("Terrain binding state IDs exhausted"); return false; }
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
    if(id>=_entries.size() || id>=_states.size() || !_entries[id].references) RADRAY_ABORT("Invalid terrain binding release: {}",id);
    auto& entry=_entries[id];
    if(--entry.references) return;
    if(_lookup.erase(entry.key)!=1) RADRAY_ABORT("Missing terrain binding lookup during release: {}",id);
    _states[id]={}; entry.nextFree=_freeHead; _freeHead=id;
}
uint32_t TerrainIndirectChunk(uint32_t remaining,uint32_t nativeLimit) noexcept { return std::min(remaining,nativeLimit); }
bool SelectTerrainSubmission(uint32_t flags,const radray::render::RenderDeviceCapabilities& caps,bool& indirect) {
    indirect=false;
    if(flags>RN_RENDERER_INDIRECT) { RADRAY_ERR_LOG("Unknown or mutually exclusive terrain submission flags: {}",flags); return false; }
    const std::string_view reason=!caps.Features.IndirectDraw?"indirect draws unsupported":
        !caps.Features.IndirectFirstInstance?"nonzero indirect firstInstance unsupported":
        caps.Limits.MaxDrawIndirectCount<=1?"native multi-record indirect unsupported":"";
    indirect=flags!=RN_RENDERER_DIRECT && reason.empty();
    if(flags==RN_RENDERER_INDIRECT && !indirect) {
        RADRAY_ERR_LOG("Terrain indirect submission unavailable: {}",reason); return false;
    }
    RADRAY_INFO_LOG("Terrain submission requested={} actual={} indirect={} first_instance={} max_native_records={} fallback={}",
        flags==RN_RENDERER_DIRECT?"direct":flags==RN_RENDERER_INDIRECT?"indirect":"auto",indirect?"indirect":"direct",
        caps.Features.IndirectDraw,caps.Features.IndirectFirstInstance,caps.Limits.MaxDrawIndirectCount,
        flags==0 && !indirect?reason:std::string_view{"none"});
    return true;
}
bool BuildTerrainDraws(TerrainFrameLists& lists,std::span<const TerrainBindingState> states,bool oit) {
    lists.arguments.clear(); lists.batches.clear(); lists.layers={};
    for(uint32_t layer=0;layer<3;++layer) {
        const bool depthFirst=layer==RN_MATERIAL_TRANSLUCENT && !oit;
        const auto& items=lists.visible[layer]; auto& order=lists.order[layer];
        if(items.size()!=order.size()) RADRAY_ABORT("Terrain payload and sort item counts differ: layer={} payload={} sort={}",layer,items.size(),order.size());
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Sort");
            Sort(order,lists.sortScratch,depthFirst);
        }
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.BuildArguments");
        auto& result=lists.layers[layer];
        result.firstBatch=uint32_t(lists.batches.size()); result.firstRecord=uint32_t(lists.arguments.size());
        for(const auto& sorted:order) {
            if(sorted.payloadIndex>=items.size()) RADRAY_ABORT("Invalid terrain payload index: layer={} index={}",layer,sorted.payloadIndex);
            const auto& item=items[sorted.payloadIndex];
            if(!item.stateId || item.stateId>=states.size() || !states[item.stateId].material ||
               !states[item.stateId].material->parameters || !item.ranges) {
                RADRAY_ABORT("Invalid terrain binding state or ranges: layer={} state={}",layer,item.stateId);
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
                    RADRAY_ERR_LOG("Terrain draw record or submitted index count overflow"); return false;
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
            RADRAY_ERR_LOG("Terrain submitted index count overflow"); return false;
        }
        submitted+=indices*multiplier;
    }
    return true;
}
}

namespace raynium {
namespace {
float TerrainCoordinate(std::span<const radray::byte> bytes,size_t vertex,uint32_t axis) noexcept {
    float value; std::memcpy(&value,bytes.data()+vertex*28+axis*4,4); return value;
}
uint32_t TerrainQuadGroup(std::span<const radray::byte> bytes,size_t first) noexcept {
    for(size_t v=0;v<4;++v) for(uint32_t a=0;a<3;++a)
        if(!std::isfinite(TerrainCoordinate(bytes,first+v,a))) return 6;
    for(uint32_t a=0;a<3;++a) {
        const auto plane=TerrainCoordinate(bytes,first,a);
        if(plane!=TerrainCoordinate(bytes,first+1,a) || plane!=TerrainCoordinate(bytes,first+2,a) || plane!=TerrainCoordinate(bytes,first+3,a)) continue;
        const uint32_t b=(a+1)%3,c=(a+2)%3;
        const double b0=TerrainCoordinate(bytes,first,b),c0=TerrainCoordinate(bytes,first,c);
        const double b1=double(TerrainCoordinate(bytes,first+1,b))-b0,c1=double(TerrainCoordinate(bytes,first+1,c))-c0;
        const double b2=double(TerrainCoordinate(bytes,first+2,b))-b0,c2=double(TerrainCoordinate(bytes,first+2,c))-c0;
        const double b3=double(TerrainCoordinate(bytes,first+3,b))-b0,c3=double(TerrainCoordinate(bytes,first+3,c))-c0;
        const double n=b1*c2-c1*b2,m=b2*c3-c2*b3;
        if(n>0 && m>0) return a*2;
        if(n<0 && m<0) return a*2+1;
    }
    return 6;
}
}
bool AnalyzeTerrainMesh(std::span<const radray::byte> bytes,rn_material_mode layer,TerrainMeshLayout& layout,
    radray::vector<radray::byte>& reordered) {
    if(layer>RN_MATERIAL_TRANSLUCENT || bytes.size()%112 || bytes.size()/28>1048576) {
        RADRAY_ERR_LOG("Invalid terrain quad stream or layer"); return false;
    }
    layout={}; layout.vertexCount=uint32_t(bytes.size()/28); layout.layer=layer; layout.finiteBounds=!bytes.empty();
    layout.minimum.fill(std::numeric_limits<double>::infinity()); layout.maximum.fill(-std::numeric_limits<double>::infinity());
    radray::array<uint32_t,7> counts{},offsets{};
    radray::vector<uint32_t> groups(bytes.size()/112);
    for(size_t q=0;q<groups.size();++q) {
        for(size_t v=0;v<4;++v) for(uint32_t a=0;a<3;++a) {
            const double x=TerrainCoordinate(bytes,q*4+v,a);
            if(!std::isfinite(x)) layout.finiteBounds=false;
            layout.minimum[a]=std::min(layout.minimum[a],x); layout.maximum[a]=std::max(layout.maximum[a],x);
        }
        groups[q]=layer==RN_MATERIAL_TRANSLUCENT?6:TerrainQuadGroup(bytes,q*4); counts[groups[q]]+=4;
    }
    uint32_t first=0;
    for(uint32_t g=0;g<7;++g) {
        offsets[g]=first/6*112;
        if(counts[g]) { layout.groupBits[g]=1u<<layout.ranges.size(); layout.ranges.push_back({first,counts[g]/4*6,0}); }
        first+=counts[g]/4*6;
    }
    reordered.resize(bytes.size());
    for(size_t q=0;q<groups.size();++q) {
        auto& offset=offsets[groups[q]]; std::memcpy(reordered.data()+offset,bytes.data()+q*112,112); offset+=112;
    }
    return true;
}
TerrainEye ComputeTerrainEye(const rn_render_frame_desc& frame) noexcept {
    const Eigen::Matrix4f matrix=Eigen::Map<const Eigen::Matrix4f>(frame.projection)*Eigen::Map<const Eigen::Matrix4f>(frame.view);
    if(!matrix.allFinite()) return {};
    const Eigen::FullPivLU<Eigen::Matrix4f> inverse(matrix);
    if(!inverse.isInvertible()) return {};
    const Eigen::Vector4f point=inverse.solve(Eigen::Vector4f(0,0,1,0));
    if(!point.allFinite() || std::abs(point.w())<=1e-8f) return {};
    const auto eye=(point/point.w()).eval();
    return {{{eye.x(),eye.y(),eye.z()}},true};
}
uint32_t TerrainVisibleMask(const TerrainMeshLayout& layout,const TerrainEye& eye,const rn_render_frame_desc& frame,
    std::span<const rn_instance> instances) noexcept {
    const uint32_t full=(1u<<layout.ranges.size())-1;
    if(layout.layer==RN_MATERIAL_TRANSLUCENT || !layout.finiteBounds || !eye.valid) return full;
    uint32_t mask=layout.groupBits[6];
    for(const auto& instance:instances) {
        radray::array<double,3> position;
        for(uint32_t a=0;a<3;++a) {
            position[a]=double(int64_t(frame.camera_block[a])-int64_t(instance.origin[a]))+frame.camera_fraction[a]+eye.offset[a];
            if(!std::isfinite(position[a])) return full;
            if(position[a]>=layout.minimum[a]-0.001) mask|=layout.groupBits[a*2];
            if(position[a]<=layout.maximum[a]+0.001) mask|=layout.groupBits[a*2+1];
        }
        if(mask==full) break;
    }
    return mask;
}
}
