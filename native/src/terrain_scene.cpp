#include <raynium/terrain_scene.h>
#include <radray/profiler.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace raynium {
using namespace radray::render;
namespace {
template<class T> bool Array(const T* p,uint32_t n,uint32_t limit=1048576) { return n<=limit && (!n || p); }
bool Batch(TerrainPrimitive& primitive,const TerrainState& state,radray::MeshBatch& batch) {
    const auto& mesh=*state.meshes.at(primitive.desc.mesh);
    batch.Material=state.materials.at(primitive.desc.material)->handle; batch.VertexInput=&mesh.geometry.GetVertexInput();
    const auto& index=*state.indices.at(mesh.indexId);
    const int64_t first=mesh.geometry.GetFirstVertex();
    for(const auto& range:mesh.ranges) {
        if(uint64_t(range.first)+range.count>index.GetIndexCount()) return false;
        const int64_t offset=int64_t(range.base_vertex)+first;
        if(offset<std::numeric_limits<int32_t>::min() || offset>std::numeric_limits<int32_t>::max()) return false;
        batch.Elements.push_back({.IndexBuffer=index.GetBuffer(),.IndexStride=index.GetIndexStride(),.FirstIndex=index.GetFirstIndex()+range.first,
            .IndexCount=range.count,.VertexOffset=int32_t(offset)});
    }
    primitive.elementCount=uint32_t(mesh.ranges.size());
    return true;
}
}
TerrainScene::TerrainScene(RayniumRuntime& device,radray::unique_ptr<radray::VertexBufferPool> vertices,
    radray::unique_ptr<radray::IndexBufferPool> indices)
    :_device(device),_vertices(std::move(vertices)),_indices(std::move(indices)) { _state.meshes.reserve(4096); _state.primitives.reserve(4096); }
bool TerrainScene::PrepareFrame(RenderTarget& target,SharedTextures* textures,const rn_render_frame_desc& f,
    const MeshPassResources& resources,SlotUploads slot,TerrainFrame& frame,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Scene.Prepare");
    RADRAY_PROFILE_PLOT("Raynium.IndexUpdates", double(f.index_count));
    RADRAY_PROFILE_PLOT("Raynium.MaterialUpdates", double(f.material_count));
    RADRAY_PROFILE_PLOT("Raynium.PrimitiveUpdates", double(f.primitive_count));
    RADRAY_PROFILE_PLOT("Raynium.PrimitiveRemovals", double(f.removed_primitive_count));
    RADRAY_PROFILE_PLOT("Raynium.Instances", double(f.instance_count));
    RADRAY_PROFILE_PLOT("Raynium.ChangedTextures", double(f.changed_textures));
    const auto fail=[&](const char* message) { error=message; return false; };
    if(f.struct_size!=sizeof(f) || f.reserved || (f.flags&~3u) || !f.epoch || f.epoch<_state.epoch ||
       !Array(f.indices,f.index_count,65536) || !Array(f.meshes,f.mesh_count,65536) || !Array(f.materials,f.material_count,65536) ||
       !Array(f.primitives,f.primitive_count,65536) || !Array(f.removed_indices,f.removed_index_count) ||
       !Array(f.removed_meshes,f.removed_mesh_count) || !Array(f.removed_materials,f.removed_material_count) ||
       !Array(f.removed_primitives,f.removed_primitive_count) || !Array(f.visible,f.visible_count) || !Array(f.instances,f.instance_count))
        return fail("Invalid frame layout, epoch or array");
    auto& state=_state;
    frame.target=&target; frame.textures=textures; frame.parameters=f;
    frame.vertices=_vertices.get(); frame.indices=_indices.get();
    auto& scalar=frame.parameters;
    scalar.indices=nullptr; scalar.meshes=nullptr; scalar.materials=nullptr; scalar.primitives=nullptr;
    scalar.removed_indices=scalar.removed_meshes=scalar.removed_materials=scalar.removed_primitives=nullptr;
    scalar.visible=nullptr; scalar.instances=nullptr;
    const bool epochChanged=f.epoch!=state.epoch;
    const bool texturesChanged=textures!=state.boundTextures;
    const bool rebuildScene=_rebuildScene || epochChanged;
    if(epochChanged && state.epoch) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Scene.RetireEpoch");
        for(const auto& [id,owner]:state.indices) slot.frame.RetireResource(owner);
        for(const auto& [id,owner]:state.meshes) slot.frame.RetireResource(owner);
        for(const auto& [id,owner]:state.materials) slot.frame.RetireResource(owner);
        _scene.Clear(); state=TerrainState{};
    } else if(rebuildScene) {
        _scene.Clear(); state.materialBindingsByHandle.clear();
    }
    if(epochChanged) { state.meshes.reserve(4096); state.primitives.reserve(4096); }
    state.epoch=f.epoch; state.boundTextures=textures; _rebuildScene=false;
    const bool hasSceneUpdates=rebuildScene || texturesChanged || f.removed_index_count || f.removed_mesh_count ||
        f.removed_material_count || f.removed_primitive_count || f.index_count || f.mesh_count || f.material_count || f.primitive_count;
    auto& device=_device.GetDevice();
    if(hasSceneUpdates) {
        const uint64_t primitives=uint64_t(_scene.GetPrimitiveCount())+f.primitive_count;
        const uint64_t batches=uint64_t(_scene.GetMeshBatchCount())+f.primitive_count;
        const uint64_t materials=uint64_t(_scene.GetMaterialCount())+f.material_count;
        if(primitives>UINT32_MAX || batches>UINT32_MAX || materials>UINT32_MAX) return fail("Terrain scene capacity exceeds 32-bit range");
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.Reserve");
            _scene.Reserve(uint32_t(primitives),uint32_t(batches),uint32_t(materials));
        }
        std::unordered_set<uint64_t> replacedIndices,replacedMeshes,updatedMaterials,touchedPrimitives;
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.UpdateResources");
            for(uint32_t i=0;i<f.removed_primitive_count;++i) {
                const auto it=state.primitives.find(f.removed_primitives[i]);
                if(it==state.primitives.end()) continue;
                if(it->second.primitiveHandle.IsValid() && !_scene.RemovePrimitive(it->second.primitiveHandle))
                    return fail("Cannot remove SOLID primitive");
                state.primitives.erase(it);
            }
            // Replaced primitives must release their old material references before material deletion.
            for(uint32_t i=0;i<f.primitive_count;++i) {
                const auto& x=f.primitives[i];
                if(!x.id || !x.mesh || !x.material) return fail("Invalid primitive");
                const auto it=state.primitives.find(x.id);
                if(it!=state.primitives.end() && it->second.primitiveHandle.IsValid()) {
                    if(!_scene.RemovePrimitive(it->second.primitiveHandle)) return fail("Cannot replace SOLID primitive");
                    it->second.primitiveHandle={}; it->second.batchHandle={};
                }
            }
            for(uint32_t i=0;i<f.removed_mesh_count;++i) {
                const auto it=state.meshes.find(f.removed_meshes[i]);
                if(it!=state.meshes.end()) { slot.frame.RetireResource(it->second); state.meshes.erase(it); }
            }
            for(uint32_t i=0;i<f.removed_index_count;++i) {
                const auto it=state.indices.find(f.removed_indices[i]);
                if(it!=state.indices.end()) { slot.frame.RetireResource(it->second); state.indices.erase(it); }
            }
            for(uint32_t i=0;i<f.removed_material_count;++i) {
                const auto it=state.materials.find(f.removed_materials[i]);
                if(it==state.materials.end()) continue;
                const auto handle=it->second->handle;
                if(handle.IsValid()) {
                    if(!_scene.RemoveMaterial(handle)) return fail("Cannot remove SOLID material");
                    state.materialBindingsByHandle.erase(handle.GetIndex());
                }
                slot.frame.RetireResource(it->second); state.materials.erase(it);
            }
        }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.StageGeometry");
            for(uint32_t i=0;i<f.index_count;++i) {
                const auto& x=f.indices[i];
                if(!x.id || x.reserved || !x.count || !Array(x.indices,x.count,16u<<20)) return fail("Invalid index resource");
                auto allocation=_indices->Allocate(x.count);
                if(!allocation) return fail("Cannot allocate terrain indices");
                auto index=std::make_shared<radray::IndexBufferSubAllocation>(std::move(*allocation));
                if(!slot.staging.Stage(index->GetBuffer(),index->GetByteOffset(),x.indices,index->GetByteSize()))
                    return fail("Cannot upload index resource");
                const auto it=state.indices.find(x.id);
                if(it!=state.indices.end()) {
                    slot.frame.RetireResource(it->second); it->second=std::move(index); replacedIndices.insert(x.id);
                } else state.indices.emplace(x.id,std::move(index));
            }
            for(uint32_t i=0;i<f.mesh_count;++i) {
                const auto& x=f.meshes[i];
                if(!x.id || !x.vertices || !x.index_buffer || !x.vertex_count || x.vertex_count>(1u<<20) || !x.submesh_count ||
                   !Array(x.submeshes,x.submesh_count,32)) return fail("Invalid SOLID mesh");
                auto mesh=std::make_shared<TerrainMesh>(); mesh->indexId=x.index_buffer; mesh->vertexCount=x.vertex_count;
                mesh->ranges.assign(x.submeshes,x.submeshes+x.submesh_count);
                auto allocation=_vertices->Allocate(x.vertex_count);
                if(!allocation) return fail("Cannot allocate terrain vertices");
                mesh->geometry=std::move(*allocation);
                if(!slot.staging.Stage(mesh->geometry.GetBuffer(),mesh->geometry.GetByteOffset(),x.vertices,mesh->geometry.GetByteSize()))
                    return fail("Cannot upload terrain vertices");
                const auto it=state.meshes.find(x.id);
                if(it!=state.meshes.end()) {
                    slot.frame.RetireResource(it->second); it->second=std::move(mesh); replacedMeshes.insert(x.id);
                } else state.meshes.emplace(x.id,std::move(mesh));
            }
        }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.MaterialBindings");
            for(uint32_t i=0;i<f.material_count;++i) {
                const auto& x=f.materials[i];
                if(!x.id || x.reserved || x.anisotropy>16) return fail("Invalid SOLID material");
                auto material=std::make_shared<TerrainMaterial>(); material->desc=x;
                const auto it=state.materials.find(x.id);
                if(it!=state.materials.end()) {
                    material->handle=it->second->handle; slot.frame.RetireResource(it->second); it->second=std::move(material);
                } else state.materials.emplace(x.id,std::move(material));
                updatedMaterials.insert(x.id);
            }
            const auto bind=[&](TerrainMaterial& material) {
                if(!textures) return fail("Material requires shared textures");
                auto binding=std::make_shared<MeshMaterialBinding>(); radray::MaterialRenderProxy proxy;
                if(!resources.CreateMaterialBinding(device,material.desc,*textures,*binding,proxy,error)) return false;
                if(rebuildScene || !material.handle.IsValid()) material.handle=_scene.AddMaterial(std::move(proxy));
                else if(!_scene.UpdateMaterial(material.handle,std::move(proxy))) return fail("Cannot update SOLID material");
                if(!material.handle.IsValid()) return fail("Cannot add SOLID material");
                if(material.binding) slot.frame.RetireResource(material.binding);
                material.binding=std::move(binding); state.materialBindingsByHandle[material.handle.GetIndex()]=material.binding;
                return true;
            };
            if(rebuildScene || texturesChanged) {
                for(auto& [id,material]:state.materials) if(!bind(*material)) return false;
            } else for(auto id:updatedMaterials) if(!bind(*state.materials.at(id))) return false;
        }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.Apply");
            const auto add=[&](TerrainPrimitive& primitive) {
                primitive.primitiveHandle=_scene.AddPrimitive();
                radray::MeshBatch batch;
                if(!Batch(primitive,state,batch)) return fail("Invalid terrain index range or vertex offset");
                primitive.batchHandle=_scene.AddMeshBatch(primitive.primitiveHandle,std::move(batch));
                if(!primitive.primitiveHandle.IsValid() || !primitive.batchHandle.IsValid()) return fail("Cannot add SOLID primitive batch");
                return true;
            };
            for(uint32_t i=0;i<f.primitive_count;++i) {
                const auto& x=f.primitives[i];
                auto& primitive=state.primitives[x.id]; primitive=TerrainPrimitive{}; primitive.desc=x;
                if(!add(primitive)) return false;
                touchedPrimitives.insert(x.id);
            }
            if(rebuildScene) {
                for(auto& [id,primitive]:state.primitives) if(!touchedPrimitives.contains(id) && !add(primitive)) return false;
            } else if(!replacedMeshes.empty() || !replacedIndices.empty()) {
                RADRAY_PROFILE_SCOPE_N("Raynium.Scene.RefreshBatches");
                for(auto& [id,primitive]:state.primitives) if(!touchedPrimitives.contains(id)) {
                    const auto& mesh=state.meshes.at(primitive.desc.mesh);
                    if(replacedMeshes.contains(primitive.desc.mesh) || replacedIndices.contains(mesh->indexId)) {
                        radray::MeshBatch batch;
                        if(!Batch(primitive,state,batch)) return fail("Invalid terrain index range or vertex offset");
                        if(!_scene.UpdateMeshBatch(primitive.batchHandle,std::move(batch))) return fail("Cannot update SOLID primitive batch");
                    }
                }
            }
        }
    }
    RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes", double(state.meshes.size()));
    RADRAY_PROFILE_PLOT("Raynium.Resident.Primitives", double(state.primitives.size()));
    RADRAY_PROFILE_PLOT("Raynium.Resident.Materials", double(state.materials.size()));
    auto& lists=slot.lists;
    lists.visibleBatches.clear();
    if(f.visible_count) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Scene.VisibleBatches");
        const auto capacity=lists.visibleBatches.capacity();
        if(capacity<f.visible_count) lists.visibleBatches.reserve(std::max(size_t(f.visible_count),capacity+capacity/2));
        for(const auto& item:std::span{f.visible,f.visible_count}) {
            const auto it=state.primitives.find(item.primitive);
            if(it==state.primitives.end() || !item.instance_count || uint64_t(item.first_instance)+item.instance_count>f.instance_count)
                return fail("Invalid visible primitive or instance range");
            const auto& primitive=it->second;
            if(!primitive.batchHandle.IsValid() || !primitive.elementCount)
                return fail("Invalid visible SOLID batch");
            const uint32_t fullMask=primitive.elementCount>=32?UINT32_MAX:((1u<<primitive.elementCount)-1u);
            if(!item.element_mask || (item.element_mask!=UINT32_MAX && (item.element_mask&~fullMask)))
                return fail("Invalid visible SOLID submesh");
            const uint32_t mask=item.element_mask==UINT32_MAX?fullMask:item.element_mask;
            lists.visibleBatches.push_back({.Batch=primitive.batchHandle,.ElementMask=mask,.VisibilitySortKey=item.sort_key,
                .FirstInstanceOverride=item.first_instance,.InstanceCountOverride=item.instance_count});
        }
    }
    frame.visibleBatches=lists.visibleBatches;
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.StageInstances");
        if(f.instance_count) {
            const uint64_t size=uint64_t(f.instance_count)*sizeof(rn_instance);
            if(!slot.instances.Reserve(device,size)) return fail("Cannot grow terrain instances");
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.CopyInstances");
                std::memcpy(slot.instances.GetData(),f.instances,size_t(size));
            }
            slot.instances.Flush(0,size);
        }
    }
    RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexPages", double(_vertices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexPages", double(_indices->GetPageCount()));
#ifdef RADRAY_ENABLE_PROFILER
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Bytes", double(slot.staging.GetStagedByteSize()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.FreeBytes", double(_vertices->GetFreeBytes()+_indices->GetFreeBytes()));
    RADRAY_PROFILE_PLOT("Raynium.Frame.StagingCapacity", double(slot.staging.GetCapacity()));
    RADRAY_PROFILE_PLOT("Raynium.Frame.InstanceCapacity", double(slot.instances.GetCapacity()));
#endif
    return true;
}
void TerrainScene::DetachTextures() noexcept {
    _scene.Clear(); _state.materialBindingsByHandle.clear();
    for(auto& [id,material]:_state.materials) { material->binding.reset(); material->handle={}; }
    for(auto& [id,primitive]:_state.primitives) { primitive.primitiveHandle={}; primitive.batchHandle={}; }
    _state.boundTextures=nullptr; _rebuildScene=true;
}
void TerrainScene::Reset() noexcept { _scene.Clear(); _state=TerrainState{}; _rebuildScene=true; }
uint64_t TerrainScene::ResidentBytes() const noexcept {
    uint64_t bytes=0;
    for(const auto& [id,index]:_state.indices) bytes+=index->GetByteSize();
    for(const auto& [id,mesh]:_state.meshes) bytes+=mesh->geometry.GetByteSize();
    return bytes;
}
}
