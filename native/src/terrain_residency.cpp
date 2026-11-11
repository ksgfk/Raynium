#include <raynium/terrain_residency.h>
#include <radray/profiler.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace raynium {
using namespace radray::render;
namespace {
template<class T> bool Array(const T* p,uint32_t n,uint32_t limit=1048576) { return n<=limit && (!n || p); }
void RemoveReference(std::unordered_map<uint64_t,std::unordered_set<uint64_t>>& map,uint64_t key,uint64_t id) {
    auto it=map.find(key); if(it==map.end()) return;
    it->second.erase(id); if(it->second.empty()) map.erase(it);
}
bool ResolveMesh(TerrainMesh& mesh,const TerrainState& state,radray::string& error) {
    const auto found=state.indices.find(mesh.indexId);
    if(found==state.indices.end()) { error="Terrain mesh references missing index resource"; return false; }
    auto draws=std::make_shared<TerrainDrawTemplate>();
    const auto& stream=mesh.geometry.GetVertexInput().Streams.front();
    draws->vertices={stream.Buffer.Get(),stream.Offset,stream.Size};
    const auto& index=*found->second;
    draws->indices=index.GetIndexBufferView();
    draws->fullMask=mesh.ranges.size()==32?UINT32_MAX:((1u<<mesh.ranges.size())-1u);
    uint64_t runCount=0;
    for(uint32_t n=0;n<mesh.ranges.size();++n) {
        const auto& range=mesh.ranges[n];
        const int64_t vertex=int64_t(range.base_vertex)+mesh.geometry.GetFirstVertex();
        const uint64_t first=uint64_t(index.GetFirstIndex())+range.first;
        if(range.reserved || uint64_t(range.first)+range.count>index.GetIndexCount() || first>UINT32_MAX ||
           vertex<std::numeric_limits<int32_t>::min() || vertex>std::numeric_limits<int32_t>::max()) {
            error="Invalid terrain index range or vertex offset"; return false;
        }
        draws->ranges[n]={uint32_t(first),range.count,int32_t(vertex)};
        if(range.count) draws->validMask|=1u<<n;
        if(n) {
            const auto& previous=draws->ranges[n-1];
            if(previous.count && range.count && previous.count%3==0 && range.count%3==0 &&
               previous.vertex==vertex && uint64_t(previous.first)+previous.count==first && runCount+range.count<=UINT32_MAX) {
                draws->mergeMask|=1u<<(n-1); runCount+=range.count;
            } else runCount=range.count;
        } else runCount=range.count;
    }
    mesh.draws=std::move(draws); return true;
}
bool ResolvePrimitive(TerrainPrimitive& primitive,const TerrainState& state,radray::string& error) {
    const auto mesh=state.meshes.find(primitive.desc.mesh),end=state.meshes.end();
    const auto material=state.materials.find(primitive.desc.material);
    if(mesh==end || material==state.materials.end() || !mesh->second->draws || !material->second->binding) {
        error="Terrain primitive references missing mesh or material"; return false;
    }
    primitive.draws=mesh->second->draws; primitive.binding=material->second->binding;
    primitive.stateSortKey=TerrainStateSortKey(*primitive.draws,*primitive.binding);
    primitive.fullMask=primitive.draws->fullMask; primitive.renderMode=material->second->desc.render_mode; return true;
}
}
TerrainResidency::TerrainResidency(RayniumRuntime& device,radray::unique_ptr<radray::VertexBufferPool> vertices,
    radray::unique_ptr<radray::IndexBufferPool> indices)
    :_device(device),_vertices(std::move(vertices)),_indices(std::move(indices)) { _state.meshes.reserve(4096); _state.primitives.reserve(4096); }
bool TerrainResidency::PrepareFrame(RenderTarget& target,SharedTextures* textures,const rn_render_frame_desc& f,
    const MeshPassResources& resources,SlotUploads slot,TerrainFrame& frame,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Prepare");
    const auto fail=[&](const char* message) { error=message; return false; };
    if(f.struct_size!=sizeof(f) || f.reserved || (f.flags&~7u) || !f.epoch || f.epoch<_state.epoch ||
       !Array(f.indices,f.index_count,65536) || !Array(f.meshes,f.mesh_count,65536) || !Array(f.materials,f.material_count,65536) ||
       !Array(f.primitives,f.primitive_count,65536) || !Array(f.removed_indices,f.removed_index_count) ||
       !Array(f.removed_meshes,f.removed_mesh_count) || !Array(f.removed_materials,f.removed_material_count) ||
       !Array(f.removed_primitives,f.removed_primitive_count) || !Array(f.visible,f.visible_count) || !Array(f.instances,f.instance_count))
        return fail("Invalid frame layout, epoch or array");
    auto& state=_state; auto& device=_device.GetDevice();
    frame.target=&target; frame.textures=textures; frame.parameters=f; frame.lists=&slot.lists;
    frame.vertices=_vertices.get(); frame.indices=_indices.get();
    auto& scalar=frame.parameters;
    scalar.indices=nullptr; scalar.meshes=nullptr; scalar.materials=nullptr; scalar.primitives=nullptr;
    scalar.removed_indices=scalar.removed_meshes=scalar.removed_materials=scalar.removed_primitives=nullptr;
    scalar.visible=nullptr; scalar.instances=nullptr;
    if(f.epoch!=state.epoch) {
        for(const auto& [id,owner]:state.indices) slot.frame.RetireResource(owner);
        for(const auto& [id,owner]:state.meshes) slot.frame.RetireResource(owner);
        for(const auto& [id,owner]:state.materials) slot.frame.RetireResource(owner);
        state=TerrainState{}; state.meshes.reserve(4096); state.primitives.reserve(4096);
    }
    const bool texturesChanged=textures!=state.boundTextures;
    state.epoch=f.epoch; state.boundTextures=textures;
    std::unordered_set<uint64_t> changedMeshes,changedIndices,changedMaterials,changedPrimitives;
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.UpdateResources");
        for(uint32_t i=0;i<f.removed_primitive_count;++i) {
            auto it=state.primitives.find(f.removed_primitives[i]); if(it==state.primitives.end()) continue;
            RemoveReference(state.primitivesByMesh,it->second.desc.mesh,it->first);
            RemoveReference(state.primitivesByMaterial,it->second.desc.material,it->first); state.primitives.erase(it);
        }
        for(uint32_t i=0;i<f.primitive_count;++i) {
            const auto& x=f.primitives[i]; if(!x.id || !x.mesh || !x.material) return fail("Invalid primitive");
            const auto it=state.primitives.find(x.id);
            if(it!=state.primitives.end()) {
                RemoveReference(state.primitivesByMesh,it->second.desc.mesh,x.id);
                RemoveReference(state.primitivesByMaterial,it->second.desc.material,x.id);
            }
            state.primitives[x.id]=TerrainPrimitive{.desc=x};
            state.primitivesByMesh[x.mesh].insert(x.id); state.primitivesByMaterial[x.material].insert(x.id);
            changedPrimitives.insert(x.id);
        }
        for(uint32_t i=0;i<f.removed_mesh_count;++i) {
            auto it=state.meshes.find(f.removed_meshes[i]); if(it==state.meshes.end()) continue;
            RemoveReference(state.meshesByIndex,it->second->indexId,it->first);
            slot.frame.RetireResource(it->second); changedMeshes.insert(it->first); state.meshes.erase(it);
        }
        for(uint32_t i=0;i<f.removed_index_count;++i) {
            auto it=state.indices.find(f.removed_indices[i]); if(it==state.indices.end()) continue;
            slot.frame.RetireResource(it->second); changedIndices.insert(it->first); state.indices.erase(it);
        }
        for(uint32_t i=0;i<f.removed_material_count;++i) {
            auto it=state.materials.find(f.removed_materials[i]); if(it==state.materials.end()) continue;
            slot.frame.RetireResource(it->second); changedMaterials.insert(it->first); state.materials.erase(it);
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.StageGeometry");
        for(uint32_t i=0;i<f.index_count;++i) {
            const auto& x=f.indices[i];
            if(!x.id || x.reserved || !x.count || !Array(x.indices,x.count,16u<<20)) return fail("Invalid index resource");
            auto allocation=_indices->Allocate(x.count); if(!allocation) return fail("Cannot allocate terrain indices");
            auto index=std::make_shared<radray::IndexBufferSubAllocation>(std::move(*allocation));
            if(!slot.staging.Stage(index->GetBuffer(),index->GetByteOffset(),x.indices,index->GetByteSize())) return fail("Cannot upload index resource");
            auto& old=state.indices[x.id]; if(old) slot.frame.RetireResource(old);
            old=std::move(index); changedIndices.insert(x.id);
        }
        for(uint32_t i=0;i<f.mesh_count;++i) {
            const auto& x=f.meshes[i];
            if(!x.id || !x.vertices || !x.index_buffer || !x.vertex_count || x.vertex_count>(1u<<20) || !x.submesh_count ||
               !Array(x.submeshes,x.submesh_count,32)) return fail("Invalid terrain mesh");
            auto mesh=std::make_shared<TerrainMesh>(); mesh->indexId=x.index_buffer; mesh->vertexCount=x.vertex_count;
            mesh->ranges.assign(x.submeshes,x.submeshes+x.submesh_count);
            auto allocation=_vertices->Allocate(x.vertex_count); if(!allocation) return fail("Cannot allocate terrain vertices");
            mesh->geometry=std::move(*allocation);
            if(!slot.staging.Stage(mesh->geometry.GetBuffer(),mesh->geometry.GetByteOffset(),x.vertices,mesh->geometry.GetByteSize())) return fail("Cannot upload terrain vertices");
            auto& old=state.meshes[x.id];
            if(old) { RemoveReference(state.meshesByIndex,old->indexId,x.id); slot.frame.RetireResource(old); }
            old=std::move(mesh); state.meshesByIndex[x.index_buffer].insert(x.id); changedMeshes.insert(x.id);
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.MaterialBindings");
        for(uint32_t i=0;i<f.material_count;++i) {
            const auto& x=f.materials[i];
            if(!x.id || x.render_mode>RN_MATERIAL_TRANSLUCENT || x.anisotropy>16) return fail("Invalid terrain material");
            auto material=std::make_shared<TerrainMaterial>(); material->desc=x;
            auto& old=state.materials[x.id]; if(old) slot.frame.RetireResource(old);
            old=std::move(material); changedMaterials.insert(x.id);
        }
        for(auto& [id,material]:state.materials) if(texturesChanged || !material->binding || changedMaterials.contains(id)) {
            if(!textures) return fail("Material requires shared textures");
            auto binding=std::make_shared<MeshMaterialBinding>();
            if(!resources.CreateMaterialBinding(device,material->desc,*textures,*binding,error)) return false;
            if(state.nextMaterialKey==UINT64_MAX) return fail("Terrain material sort key exhausted");
            binding->sortKey=++state.nextMaterialKey;
            if(material->binding) slot.frame.RetireResource(material->binding);
            material->binding=std::move(binding); changedMaterials.insert(id);
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.ResolveTemplates");
        for(auto id:changedIndices) {
            auto refs=state.meshesByIndex.find(id);
            if(refs!=state.meshesByIndex.end()) changedMeshes.insert(refs->second.begin(),refs->second.end());
        }
        for(auto id:changedMeshes) {
            auto mesh=state.meshes.find(id);
            if(mesh!=state.meshes.end() && !ResolveMesh(*mesh->second,state,error)) return false;
            auto refs=state.primitivesByMesh.find(id);
            if(refs!=state.primitivesByMesh.end()) changedPrimitives.insert(refs->second.begin(),refs->second.end());
        }
        for(auto id:changedMaterials) {
            auto refs=state.primitivesByMaterial.find(id);
            if(refs!=state.primitivesByMaterial.end()) changedPrimitives.insert(refs->second.begin(),refs->second.end());
        }
        for(auto id:changedPrimitives) if(!ResolvePrimitive(state.primitives.at(id),state,error)) return false;
    }
    for(auto& layer:slot.lists.visible) layer.clear();
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.ResolveVisible");
        for(const auto& item:std::span{f.visible,f.visible_count}) {
            const auto it=state.primitives.find(item.primitive);
            if(it==state.primitives.end() || !item.instance_count || uint64_t(item.first_instance)+item.instance_count>f.instance_count)
                return fail("Invalid visible primitive or instance range");
            const auto& primitive=it->second;
            if(!primitive.draws || !primitive.binding || !item.element_mask ||
               (item.element_mask!=UINT32_MAX && (item.element_mask&~primitive.fullMask))) return fail("Invalid visible terrain submesh");
            slot.lists.visible[primitive.renderMode].push_back({primitive.draws.get(),primitive.binding.get(),
                (uint64_t(primitive.stateSortKey)<<32)|item.sort_key,
                item.element_mask==UINT32_MAX?primitive.fullMask:item.element_mask,item.first_instance,item.instance_count});
        }
    }
    if(!BuildTerrainDraws(slot.lists,(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)!=0,error)) return false;
    frame.hasTranslucent=!slot.lists.visible[RN_MATERIAL_TRANSLUCENT].empty();
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.StageInstances");
        const uint64_t size=uint64_t(f.instance_count)*sizeof(rn_instance);
        if(size) {
            if(!slot.instances.Reserve(device,size)) return fail("Cannot grow terrain instances");
            std::memcpy(slot.instances.GetData(),f.instances,size_t(size));
            if(!slot.instances.Flush(0,size)) return fail("Cannot flush terrain instances");
        }
    }
    if(slot.useIndirect && !slot.lists.arguments.empty()) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.UploadArguments");
        const uint64_t bytes=slot.lists.arguments.size()*sizeof(DrawIndexedIndirectArguments);
        if(!slot.indirect.Reserve(device,bytes)) return fail("Cannot allocate or map terrain indirect arguments");
        std::memcpy(slot.indirect.GetData(),slot.lists.arguments.data(),size_t(bytes));
        if(!slot.indirect.Flush(0,bytes)) return fail("Cannot flush terrain indirect arguments");
    }
    RADRAY_PROFILE_PLOT("Raynium.Visible.Solid",double(slot.lists.visible[0].size()));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Cutout",double(slot.lists.visible[1].size()));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Translucent",double(slot.lists.visible[2].size()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.ArgumentBytes",double(slot.lists.arguments.size()*sizeof(DrawIndexedIndirectArguments)));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.UploadedArgumentBytes",double(slot.useIndirect?slot.lists.arguments.size()*sizeof(DrawIndexedIndirectArguments):0));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltRecords",double(slot.lists.arguments.size()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltBatches",double(slot.lists.batches.size()));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.OitParameterBuilds",double(frame.hasTranslucent && (f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)?1:0));
#ifdef RADRAY_ENABLE_PROFILER
    const std::array<const char*,3> groups{"Raynium.Terrain.Solid.SelectedGroups","Raynium.Terrain.Cutout.SelectedGroups","Raynium.Terrain.Translucent.SelectedGroups"};
    const std::array<const char*,3> records{"Raynium.Terrain.Solid.Records","Raynium.Terrain.Cutout.Records","Raynium.Terrain.Translucent.Records"};
    const std::array<const char*,3> batches{"Raynium.Terrain.Solid.Batches","Raynium.Terrain.Cutout.Batches","Raynium.Terrain.Translucent.Batches"};
    for(size_t layer=0;layer<3;++layer) {
        RADRAY_PROFILE_PLOT(groups[layer],double(slot.lists.layers[layer].selectedGroups));
        RADRAY_PROFILE_PLOT(records[layer],double(slot.lists.layers[layer].recordCount));
        RADRAY_PROFILE_PLOT(batches[layer],double(slot.lists.layers[layer].batchCount));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes",double(state.meshes.size()));
    RADRAY_PROFILE_PLOT("Raynium.Resident.Primitives",double(state.primitives.size()));
    RADRAY_PROFILE_PLOT("Raynium.Resident.Materials",double(state.materials.size()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexPages",double(_vertices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexPages",double(_indices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Bytes",double(slot.staging.GetStagedByteSize()));
    return true;
}
void TerrainResidency::DetachTextures() noexcept {
    for(auto& [id,primitive]:_state.primitives) primitive.binding.reset();
    for(auto& [id,material]:_state.materials) material->binding.reset();
    _state.boundTextures=nullptr;
}
void TerrainResidency::Reset() noexcept { _state=TerrainState{}; }
uint64_t TerrainResidency::ResidentBytes() const noexcept {
    uint64_t bytes=0;
    for(const auto& [id,index]:_state.indices) bytes+=index->GetByteSize();
    for(const auto& [id,mesh]:_state.meshes) bytes+=mesh->geometry.GetByteSize();
    return bytes;
}
}
