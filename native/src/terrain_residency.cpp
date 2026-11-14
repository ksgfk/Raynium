#include <raynium/terrain_residency.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::render;
namespace {
template<class T> bool Array(const T* p,uint32_t n,uint32_t limit=1048576) { return n<=limit && (!n || p); }
bool ResolveMesh(TerrainMesh& mesh,TerrainState& state) {
    const auto& index=mesh.layout.layer==RN_MATERIAL_TRANSLUCENT?mesh.indices:state.quadIndices;
    if(!index) { RADRAY_ERR_LOG("Terrain mesh references missing indices"); return false; }
    TerrainDrawTemplate draws;
    const auto& streams=mesh.geometry.GetVertexInput().Streams;
    if(streams.empty() || mesh.layout.ranges.size()>=32) RADRAY_ABORT("Invalid terrain geometry stream or range layout");
    const auto& stream=streams.front();
    draws.vertices={stream.Buffer.Get(),stream.Offset,stream.Size}; draws.indices=index.GetIndexBufferView();
    draws.vertexPage=mesh.geometry.GetGpuBuffer().Get(); draws.indexPage=index.GetGpuBuffer().Get();
    draws.fullMask=(1u<<mesh.layout.ranges.size())-1;
    draws.ranges.reserve(mesh.layout.ranges.size());
    for(uint32_t n=0;n<mesh.layout.ranges.size();++n) {
        const auto& range=mesh.layout.ranges[n];
        const uint64_t first=uint64_t(index.GetFirstIndex())+range.first;
        const uint64_t vertex=mesh.geometry.GetFirstVertex();
        if(uint64_t(range.first)+range.count>index.GetIndexCount() || first>UINT32_MAX || vertex>INT32_MAX) {
            RADRAY_ERR_LOG("Invalid terrain index range or vertex offset"); return false;
        }
        draws.ranges.push_back({uint32_t(first),range.count,int32_t(vertex)});
        if(range.count) draws.validMask|=1u<<n;
        if(n && mesh.layout.layer!=RN_MATERIAL_TRANSLUCENT) draws.mergeMask|=1u<<(n-1);
    }
    state.bindings.Release(mesh.stateId); mesh.stateId=0; mesh.draws=std::move(draws);
    if(state.material && !state.bindings.Acquire(mesh.draws,*state.material,mesh.stateId)) return false;
    return true;
}
}
TerrainResidency::TerrainResidency(radray::DeviceContext& device,radray::unique_ptr<radray::VertexBufferPool> vertices,
    radray::unique_ptr<radray::IndexBufferPool> indices)
    :_device(device),_vertices(std::move(vertices)),_indices(std::move(indices)) { _state.meshes.reserve(4096); }
bool TerrainResidency::PrepareFrame(const UploadedTextures& uploaded,const rn_render_frame_desc& f,
    const MeshPassResources& resources,SlotUploads slot,TerrainFrame& frame) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Prepare");

    if(f.struct_size!=sizeof(f) || (f.flags&~15u) || !f.epoch || f.epoch<_state.epoch ||
       !Array(f.indices,f.index_count,65536) || !Array(f.meshes,f.mesh_count,65536) ||
       !Array(f.removed_meshes,f.removed_mesh_count) || !Array(f.visible,f.visible_count) || !Array(f.instances,f.instance_count))
        { RADRAY_ERR_LOG("Invalid frame layout, epoch or array: sequence={} epoch={} resident_epoch={}",f.output_sequence,f.epoch,_state.epoch); return false; }
    auto& state=_state; auto& device=_device.GetDevice();
    frame.parameters=f; frame.lists=&slot.lists; frame.geometry=&slot.geometry; frame.instances={f.instances,f.instance_count};
    auto& scalar=frame.parameters;
    scalar.indices=nullptr; scalar.meshes=nullptr; scalar.removed_meshes=nullptr; scalar.visible=nullptr; scalar.instances=nullptr;
    scalar.gui_vertices=nullptr; scalar.gui_draws=nullptr;
    if(f.epoch!=state.epoch) {
        for(auto& [id,owner]:state.meshes) slot.frame.Emplace<TerrainMesh>(std::move(owner));
        if(state.quadIndices) slot.frame.Emplace<radray::IndexBufferSubAllocation>(std::move(state.quadIndices));
        if(state.material) slot.frame.Emplace<radray::unique_ptr<MeshMaterialBinding>>(std::move(state.material));
        state=TerrainState{}; state.meshes.reserve(4096);
    }
    state.epoch=f.epoch;
    std::pmr::unordered_set<uint64_t> changedMeshes{&slot.frame.GetScratchMemoryResource()};
    uint64_t uploadBytes=0;
    for(uint32_t i=0;i<f.removed_mesh_count;++i) {
        auto it=state.meshes.find(f.removed_meshes[i]); if(it==state.meshes.end()) continue;
        state.bindings.Release(it->second.stateId); slot.frame.Emplace<TerrainMesh>(std::move(it->second)); state.meshes.erase(it);
    }
    uint32_t needed=state.quadVertexCapacity;
    for(const auto& x:std::span{f.meshes,f.mesh_count}) {
        if(!x.id || !x.vertices || !x.vertex_count || x.vertex_count%4 || x.vertex_count>1048576 || x.layer>RN_MATERIAL_TRANSLUCENT)
            { RADRAY_ERR_LOG("Invalid terrain mesh: id={} vertices={} layer={}",x.id,x.vertex_count,uint32_t(x.layer)); return false; }
        if(x.layer!=RN_MATERIAL_TRANSLUCENT) needed=std::max(needed,x.vertex_count);
    }
    if(needed>state.quadVertexCapacity) {
        uint32_t capacity=65536; while(capacity<needed) capacity*=2;
        auto allocation=_indices->Allocate(capacity/4*6); if(!allocation) { RADRAY_ERR_LOG("Cannot allocate shared quad indices"); return false; }
        auto& owner=*allocation;
        uploadBytes+=owner.GetByteSize();
        slot.geometry.push_back({owner.GetGpuBuffer(),owner.GetByteOffset(),radray::RDGUploadSource::Generated(owner.GetByteSize(),capacity,
            +[](const uint32_t& vertices,radray::RDGUploadWriteContext& context) noexcept { return WriteQuadIndices(context.Bytes,vertices); })});
        if(_indexPages.insert(owner.GetBuffer()).second) _indexReservedBytes+=owner.GetBuffer()->GetDesc().Size;
        if(state.quadIndices) slot.frame.Emplace<radray::IndexBufferSubAllocation>(std::move(state.quadIndices));
        state.quadIndices=std::move(owner); state.quadVertexCapacity=capacity;
        for(const auto& [id,mesh]:state.meshes) if(mesh.layout.layer!=RN_MATERIAL_TRANSLUCENT) changedMeshes.insert(id);
    }
    for(const auto& x:std::span{f.meshes,f.mesh_count}) {
        TerrainMesh mesh;
        auto* upload=slot.frame.Emplace<TerrainMeshUpload>(slot.frame.GetMemoryResource());
        if(!AnalyzeTerrainMesh({static_cast<const radray::byte*>(x.vertices),size_t(x.vertex_count)*28},x.layer,mesh.layout,*upload)) return false;
        auto allocation=_vertices->Allocate(x.vertex_count); if(!allocation) { RADRAY_ERR_LOG("Cannot allocate terrain vertices"); return false; }
        mesh.geometry=std::move(*allocation);
        if(_vertexPages.insert(mesh.geometry.GetBuffer()).second) _vertexReservedBytes+=mesh.geometry.GetBuffer()->GetDesc().Size;
        uploadBytes+=upload->source.size();
        slot.geometry.push_back({mesh.geometry.GetGpuBuffer(),mesh.geometry.GetByteOffset(),radray::RDGUploadSource::Generated(upload->source.size(),upload,
            +[](TerrainMeshUpload* const& source,radray::RDGUploadWriteContext& context) noexcept { return source->Write(context.Bytes); })});
        auto [entry,inserted]=state.meshes.try_emplace(x.id);
        auto& previous=entry->second;
        if(!inserted) {
            // The surviving index interval is retired only when its newest owner stops using it.
            if(x.layer==RN_MATERIAL_TRANSLUCENT && previous.layout.layer==x.layer) mesh.indices=std::move(previous.indices);
            state.bindings.Release(previous.stateId); slot.frame.Emplace<TerrainMesh>(std::move(previous));
        }
        previous=std::move(mesh); changedMeshes.insert(x.id);
    }
    for(const auto& x:std::span{f.indices,f.index_count}) {
        auto found=state.meshes.find(x.mesh_id);
        if(!x.mesh_id || found==state.meshes.end() || found->second.layout.layer!=RN_MATERIAL_TRANSLUCENT || x.reserved ||
           !Array(x.indices,x.count,16u<<20) || x.count!=found->second.layout.vertexCount/4*6)
            { RADRAY_ERR_LOG("Invalid transparent index update: mesh={} count={}",x.mesh_id,x.count); return false; }
        auto allocation=_indices->Allocate(x.count); if(!allocation) { RADRAY_ERR_LOG("Cannot allocate terrain indices"); return false; }
        auto& owner=*allocation;
        if(_indexPages.insert(owner.GetBuffer()).second) _indexReservedBytes+=owner.GetBuffer()->GetDesc().Size;
        uploadBytes+=owner.GetByteSize();
        slot.geometry.push_back({owner.GetGpuBuffer(),owner.GetByteOffset(),
            radray::RDGUploadSource::BorrowedBytes(std::as_bytes(std::span{x.indices,x.count}))});
        auto& mesh=found->second; if(mesh.indices) slot.frame.Emplace<radray::IndexBufferSubAllocation>(std::move(mesh.indices));
        mesh.indices=std::move(owner); changedMeshes.insert(x.mesh_id);
    }
    bool materialChanged=false;
    if(f.visible_count) {
        if(f.terrain_anisotropy>16 || f.terrain_albedo_texture<16 || f.terrain_light_texture<16 ||
           !uploaded.Find(f.terrain_albedo_texture) || !uploaded.Find(f.terrain_light_texture)) { RADRAY_ERR_LOG("Invalid terrain texture binding: albedo={} light={} anisotropy={}",f.terrain_albedo_texture,f.terrain_light_texture,f.terrain_anisotropy); return false; }
        materialChanged=!state.material || state.albedo!=f.terrain_albedo_texture || state.light!=f.terrain_light_texture || state.anisotropy!=f.terrain_anisotropy;
        if(materialChanged) {
            auto binding=radray::make_unique<MeshMaterialBinding>();
            if(!resources.CreateMaterialBinding(device,f,uploaded,*binding)) return false;
            if(state.nextMaterialKey==UINT64_MAX) { RADRAY_ERR_LOG("Terrain material sort key exhausted"); return false; }
            binding->sortKey=++state.nextMaterialKey;
            if(state.material) slot.frame.Emplace<radray::unique_ptr<MeshMaterialBinding>>(std::move(state.material));
            state.material=std::move(binding); state.albedo=f.terrain_albedo_texture; state.light=f.terrain_light_texture; state.anisotropy=f.terrain_anisotropy;
        }
    }
    for(auto id:changedMeshes) if(!ResolveMesh(state.meshes.at(id),state)) return false;
    if(materialChanged) for(auto& [id,mesh]:state.meshes) if(!changedMeshes.contains(id)) {
        state.bindings.Release(mesh.stateId); mesh.stateId=0;
        if(!state.bindings.Acquire(mesh.draws,*state.material,mesh.stateId)) return false;
    }
    for(auto& layer:slot.lists.visible) layer.clear();
    for(auto& layer:slot.lists.order) layer.clear();
    const auto eye=ComputeTerrainEye(f);
    for(const auto& item:std::span{f.visible,f.visible_count}) {
        auto found=state.meshes.find(item.mesh);
        if(found==state.meshes.end() || !item.instance_count || uint64_t(item.first_instance)+item.instance_count>f.instance_count)
            { RADRAY_ERR_LOG("Invalid visible mesh or instance range: mesh={} first={} count={}",item.mesh,item.first_instance,item.instance_count); return false; }
        const auto& mesh=found->second; const auto& draws=mesh.draws;
        if(!mesh.stateId || !item.element_mask || (item.element_mask!=UINT32_MAX && (item.element_mask&~draws.fullMask)))
            { RADRAY_ERR_LOG("Invalid visible terrain submesh: mesh={} mask={}",item.mesh,item.element_mask); return false; }
        const uint32_t requested=item.element_mask==UINT32_MAX?draws.fullMask:item.element_mask;
        const uint32_t mask=requested&draws.validMask&TerrainVisibleMask(mesh.layout,eye,f,{f.instances+item.first_instance,item.instance_count});
        if(!mask) continue;
        auto& visible=slot.lists.visible[mesh.layout.layer]; auto& order=slot.lists.order[mesh.layout.layer];
        order.push_back({(uint64_t(mesh.stateId)<<32)|item.sort_key,uint32_t(visible.size())});
        visible.push_back({draws.ranges.data(),mask,draws.mergeMask,item.first_instance,item.instance_count,mesh.stateId});
    }
    if(!BuildTerrainDraws(slot.lists,state.bindings.States(),(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)!=0)) return false;
    frame.hasTranslucent=!slot.lists.visible[RN_MATERIAL_TRANSLUCENT].empty();
    RADRAY_PROFILE_PLOT("Raynium.Visible.Solid",double(slot.lists.visible[0].size()));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Cutout",double(slot.lists.visible[1].size()));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Translucent",double(slot.lists.visible[2].size()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.ArgumentBytes",double(slot.lists.arguments.size()*sizeof(DrawIndexedIndirectArguments)));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.UploadedArgumentBytes",double(slot.useIndirect?slot.lists.arguments.size()*sizeof(DrawIndexedIndirectArguments):0));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltRecords",double(slot.lists.arguments.size()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltBatches",double(slot.lists.batches.size()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.OitParameterBuilds",double(frame.hasTranslucent && (f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)?1:0));
#ifdef RADRAY_ENABLE_PROFILER
    const radray::array<const char*,3> groups{"Raynium.Terrain.Solid.SelectedGroups","Raynium.Terrain.Cutout.SelectedGroups","Raynium.Terrain.Translucent.SelectedGroups"};
    const radray::array<const char*,3> records{"Raynium.Terrain.Solid.Records","Raynium.Terrain.Cutout.Records","Raynium.Terrain.Translucent.Records"};
    const radray::array<const char*,3> batches{"Raynium.Terrain.Solid.Batches","Raynium.Terrain.Cutout.Batches","Raynium.Terrain.Translucent.Batches"};
    for(size_t layer=0;layer<3;++layer) {
        RADRAY_PROFILE_PLOT(groups[layer],double(slot.lists.layers[layer].selectedGroups));
        RADRAY_PROFILE_PLOT(records[layer],double(slot.lists.layers[layer].recordCount));
        RADRAY_PROFILE_PLOT(batches[layer],double(slot.lists.layers[layer].batchCount));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes",double(state.meshes.size()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexPages",double(_vertices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexPages",double(_indices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BindingStates",double(state.bindings.Count()));
#ifdef RADRAY_ENABLE_PROFILER
    if(f.output_sequence%120==0) {
        const auto vertexFree=_vertices->GetFreeBytes(),indexFree=_indices->GetFreeBytes();
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexReservedBytes",double(_vertexReservedBytes));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexReservedBytes",double(_indexReservedBytes));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexFreeBytes",double(vertexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexFreeBytes",double(indexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexAllocatedBytes",double(_vertexReservedBytes-vertexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexAllocatedBytes",double(_indexReservedBytes-indexFree));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Bytes",double(uploadBytes));
    return true;
}
void TerrainResidency::Reset() noexcept try { _state=TerrainState{}; }
catch(...) { AbortUnexpectedException("TerrainResidency::Reset"); }
uint64_t TerrainResidency::ResidentBytes() const noexcept {
    uint64_t bytes=_state.quadIndices?_state.quadIndices.GetByteSize():0;
    for(const auto& [id,mesh]:_state.meshes) {
        bytes+=mesh.geometry.GetByteSize(); if(mesh.indices) bytes+=mesh.indices.GetByteSize();
    }
    return bytes;
}
}
