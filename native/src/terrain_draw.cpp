#include <raynium/terrain_draw.h>
#include <raynium/mesh_pass.h>
#include <radray/profiler.h>
#include <algorithm>
#include <bit>
#include <limits>

namespace raynium {
namespace {
uint64_t Mix(uint64_t value) noexcept {
    value^=value>>30; value*=0xbf58476d1ce4e5b9ULL;
    value^=value>>27; value*=0x94d049bb133111ebULL; return value^(value>>31);
}
void Sort(std::vector<TerrainVisible>& items,std::vector<TerrainVisible>& scratch,bool depthFirst) {
    const auto key=[depthFirst](const TerrainVisible& item) { return depthFirst?std::rotl(item.sortKey,32):item.sortKey; };
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
bool Same(const TerrainDrawBatch& batch,const TerrainVisible& item) {
    const auto& v=item.geometry->vertices; const auto& i=item.geometry->indices;
    return batch.material->parameters.get()==item.material->parameters.get() &&
        batch.vertices.Target==v.Target && batch.vertices.Offset==v.Offset && batch.vertices.Size==v.Size &&
        batch.indices.Target==i.Target && batch.indices.Offset==i.Offset && batch.indices.Stride==i.Stride;
}
}
uint32_t TerrainStateSortKey(const TerrainDrawTemplate& geometry,const MeshMaterialBinding& material) noexcept {
    const auto& view=geometry.vertices;
    const auto vertex=uint32_t(Mix(reinterpret_cast<uintptr_t>(view.Target))^Mix(view.Offset)^Mix(view.Size))&4095u;
    const uint32_t binding=material.sortKey<=0xfffffu?uint32_t(material.sortKey):1u+uint32_t(Mix(material.sortKey)%0xfffffu);
    return (binding<<12)|vertex;
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
bool BuildTerrainDraws(TerrainFrameLists& lists,bool oit,radray::string& error) {
    lists.arguments.clear(); lists.batches.clear(); lists.layers={};
    for(uint32_t layer=0;layer<3;++layer) {
        const bool depthFirst=layer==RN_MATERIAL_TRANSLUCENT && !oit;
        auto& items=lists.visible[layer];
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Sort");
            Sort(items,lists.sortScratch,depthFirst);
        }
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.BuildArguments");
        auto& result=lists.layers[layer];
        result.firstBatch=uint32_t(lists.batches.size()); result.firstRecord=uint32_t(lists.arguments.size());
        for(const auto& item:items) {
            auto mask=item.mask&item.geometry->validMask;
            result.selectedGroups+=std::popcount(mask);
            if(!mask) continue;
            if(lists.batches.size()==result.firstBatch || !Same(lists.batches.back(),item))
                lists.batches.push_back({item.geometry->vertices,item.geometry->indices,item.material,uint32_t(lists.arguments.size()),0});
            while(mask) {
                auto bit=uint32_t(std::countr_zero(mask)); mask&=mask-1;
                const auto& range=item.geometry->ranges[bit];
                auto count=range.count;
                if(!depthFirst && item.instanceCount==1) {
                    while(bit<31 && (item.geometry->mergeMask&(1u<<bit)) && (mask&(1u<<(bit+1))) &&
                          uint64_t(count)+item.geometry->ranges[bit+1].count<=std::numeric_limits<uint32_t>::max()) {
                        count+=item.geometry->ranges[++bit].count; mask&=~(1u<<bit);
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
