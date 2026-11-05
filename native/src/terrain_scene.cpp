#include <raynium/terrain_scene.h>
#include <radray/profiler.h>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace raynium {
using namespace radray::render;
namespace {
constexpr uint64_t Align16(uint64_t value) noexcept { return (value+15)&~uint64_t(15); }
template<class T> bool Array(const T* p,uint32_t n,uint32_t limit=1048576) { return n<=limit && (!n || p); }
// Copies one record into the slot's mapped staging range; the record is copied to the GPU during Record.
bool Stage(MappedUploadBuffer& staging,uint64_t& cursor,radray::render::Buffer* destination,uint64_t destinationOffset,
    const void* data,uint64_t bytes,BufferStates after,TerrainFrame& frame) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Geometry.StageUpload");
    const uint64_t offset=Align16(cursor);
    if(bytes && (!data || offset+bytes>staging.Capacity())) return false;
    if(bytes) std::memcpy(staging.Data()+offset,data,size_t(bytes));
    frame.uploads.push_back({destination,destinationOffset,offset,bytes,after});
    cursor=offset+bytes; return true;
}
bool Batch(const TerrainPrimitive& primitive,const TerrainState& state,radray::MeshBatch& batch) {
    const auto& mesh=*state.meshes.at(primitive.desc.mesh);
    batch.Material=state.materials.at(primitive.desc.material)->handle; batch.VertexInput=&mesh.geometry.Input();
    auto* index=state.indices.at(mesh.indexId)->buffer.get();
    const int64_t first=mesh.geometry.FirstVertex();
    for(const auto& range:mesh.ranges) {
        const int64_t offset=int64_t(range.base_vertex)+first;
        if(offset<std::numeric_limits<int32_t>::min() || offset>std::numeric_limits<int32_t>::max()) return false;
        batch.Elements.push_back({.IndexBuffer=index,.FirstIndex=range.first,
            .IndexCount=range.count,.VertexOffset=int32_t(offset)});
    }
    return true;
}
}
TerrainScene::TerrainScene(RayniumRuntime& device,radray::unique_ptr<GeometryArena> arena)
    :_device(device),_arena(std::move(arena)) {}
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
    auto& scalar=frame.parameters;
    scalar.indices=nullptr; scalar.meshes=nullptr; scalar.materials=nullptr; scalar.primitives=nullptr;
    scalar.removed_indices=scalar.removed_meshes=scalar.removed_materials=scalar.removed_primitives=nullptr;
    scalar.visible=nullptr; scalar.instances=nullptr;
    const bool epochChanged=f.epoch!=state.epoch;
    const bool texturesChanged=textures!=state.boundTextures;
    const bool rebuildScene=_rebuildScene || epochChanged;
    if(epochChanged && state.epoch) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Scene.RetireEpoch");
        for(const auto& [id,owner]:state.indices) _retiredResources.emplace_back(owner);
        for(const auto& [id,owner]:state.meshes) _retiredResources.emplace_back(owner);
        for(const auto& [id,owner]:state.materials) _retiredResources.emplace_back(owner);
        _scene.Clear(); state=TerrainState{};
    } else if(rebuildScene) {
        _scene.Clear(); state.materialBindingsByHandle.clear();
    }
    state.epoch=f.epoch; state.boundTextures=textures; _rebuildScene=false;
    const bool hasSceneUpdates=rebuildScene || texturesChanged || f.removed_index_count || f.removed_mesh_count ||
        f.removed_material_count || f.removed_primitive_count || f.index_count || f.mesh_count || f.material_count || f.primitive_count;
    auto& device=_device.GetDevice();
    if(hasSceneUpdates) {
        std::unordered_set<uint64_t> replacedIndices,replacedMeshes,updatedMaterials,touchedPrimitives;
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.UpdateResources");
            for(uint32_t i=0;i<f.removed_primitive_count;++i) {
                const auto it=state.primitives.find(f.removed_primitives[i]);
                if(it==state.primitives.end()) continue;
                if(it->second->primitiveHandle.IsValid() && !_scene.RemovePrimitive(it->second->primitiveHandle))
                    return fail("Cannot remove SOLID primitive");
                state.primitives.erase(it);
            }
            // Replaced primitives must release their old material references before material deletion.
            for(uint32_t i=0;i<f.primitive_count;++i) {
                const auto& x=f.primitives[i];
                if(!x.id || !x.mesh || !x.material) return fail("Invalid primitive");
                const auto it=state.primitives.find(x.id);
                if(it!=state.primitives.end() && it->second->primitiveHandle.IsValid()) {
                    if(!_scene.RemovePrimitive(it->second->primitiveHandle)) return fail("Cannot replace SOLID primitive");
                    it->second->primitiveHandle={}; it->second->batchHandle={};
                }
            }
            for(uint32_t i=0;i<f.removed_mesh_count;++i) {
                const auto it=state.meshes.find(f.removed_meshes[i]);
                if(it!=state.meshes.end()) { _retiredResources.emplace_back(it->second); state.meshes.erase(it); }
            }
            for(uint32_t i=0;i<f.removed_index_count;++i) {
                const auto it=state.indices.find(f.removed_indices[i]);
                if(it!=state.indices.end()) { _retiredResources.emplace_back(it->second); state.indices.erase(it); }
            }
            for(uint32_t i=0;i<f.removed_material_count;++i) {
                const auto it=state.materials.find(f.removed_materials[i]);
                if(it==state.materials.end()) continue;
                const auto handle=it->second->handle;
                if(handle.IsValid()) {
                    if(!_scene.RemoveMaterial(handle)) return fail("Cannot remove SOLID material");
                    state.materialBindingsByHandle.erase(handle.Index);
                }
                _retiredResources.emplace_back(it->second); state.materials.erase(it);
            }
        }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.StageGeometry");
            frame.uploads.reserve(size_t(f.index_count)+f.mesh_count);
            // Size the slot staging range before any upload is copied; items are 16-byte aligned.
            uint64_t required=0;
            for(uint32_t i=0;i<f.index_count;++i) {
                const auto& x=f.indices[i];
                if(!x.id || x.reserved || !x.count || !Array(x.indices,x.count,16u<<20)) return fail("Invalid index resource");
                required+=Align16(uint64_t(x.count)*4);
            }
            for(uint32_t i=0;i<f.mesh_count;++i) {
                const auto& x=f.meshes[i];
                if(!x.id || !x.vertices || !x.index_buffer || !x.vertex_count || x.vertex_count>(1u<<20) || !x.submesh_count ||
                   !Array(x.submeshes,x.submesh_count,65536)) return fail("Invalid SOLID mesh");
                required+=Align16(uint64_t(x.vertex_count)*GeometryArena::VertexStride);
            }
            if(!slot.staging.Reserve(device,required,error)) return false;
            uint64_t cursor=0;
            for(uint32_t i=0;i<f.index_count;++i) {
                const auto& x=f.indices[i];
                auto index=std::make_shared<TerrainIndex>(); const uint64_t bytes=uint64_t(x.count)*4;
                index->buffer=CreateBuffer(device,bytes,MemoryType::Device,BufferUse::CopyDestination|BufferUse::Index);
                if(!index->buffer || !Stage(slot.staging,cursor,index->buffer.get(),0,x.indices,bytes,BufferState::Index,frame))
                    return fail("Cannot upload index resource");
                const auto it=state.indices.find(x.id);
                if(it!=state.indices.end()) {
                    _retiredResources.emplace_back(it->second); it->second=std::move(index); replacedIndices.insert(x.id);
                } else state.indices.emplace(x.id,std::move(index));
            }
            for(uint32_t i=0;i<f.mesh_count;++i) {
                const auto& x=f.meshes[i];
                auto mesh=std::make_shared<TerrainMesh>(); mesh->indexId=x.index_buffer; mesh->vertexCount=x.vertex_count;
                mesh->ranges.assign(x.submeshes,x.submeshes+x.submesh_count);
                auto allocation=_arena->Allocate(x.vertex_count,error);
                if(!allocation) return false;
                mesh->geometry=std::move(*allocation);
                if(!Stage(slot.staging,cursor,mesh->geometry.VertexBuffer(),mesh->geometry.ByteOffset(),x.vertices,
                       uint64_t(x.vertex_count)*GeometryArena::VertexStride,BufferState::Vertex,frame))
                    return fail("Cannot upload terrain vertices");
                const auto it=state.meshes.find(x.id);
                if(it!=state.meshes.end()) {
                    _retiredResources.emplace_back(it->second); it->second=std::move(mesh); replacedMeshes.insert(x.id);
                } else state.meshes.emplace(x.id,std::move(mesh));
            }
            if(cursor) slot.staging.Flush(0,cursor);
        }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Scene.MaterialBindings");
            for(uint32_t i=0;i<f.material_count;++i) {
                const auto& x=f.materials[i];
                if(!x.id || x.reserved || x.anisotropy>16) return fail("Invalid SOLID material");
                auto material=std::make_shared<TerrainMaterial>(); material->desc=x;
                const auto it=state.materials.find(x.id);
                if(it!=state.materials.end()) {
                    material->handle=it->second->handle; _retiredResources.emplace_back(it->second); it->second=std::move(material);
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
                if(material.binding) _retiredResources.emplace_back(material.binding);
                material.binding=std::move(binding); state.materialBindingsByHandle[material.handle.Index]=material.binding;
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
                if(!Batch(primitive,state,batch)) return fail("Terrain vertex offset exceeds 32-bit range");
                primitive.batchHandle=_scene.AddMeshBatch(primitive.primitiveHandle,std::move(batch));
                if(!primitive.primitiveHandle.IsValid() || !primitive.batchHandle.IsValid()) return fail("Cannot add SOLID primitive batch");
                return true;
            };
            for(uint32_t i=0;i<f.primitive_count;++i) {
                const auto& x=f.primitives[i]; auto primitive=std::make_shared<TerrainPrimitive>(); primitive->desc=x;
                state.primitives[x.id]=primitive;
                if(!add(*primitive)) return false;
                touchedPrimitives.insert(x.id);
            }
            if(rebuildScene) {
                for(auto& [id,primitive]:state.primitives) if(!touchedPrimitives.contains(id) && !add(*primitive)) return false;
            } else if(!replacedMeshes.empty() || !replacedIndices.empty()) {
                RADRAY_PROFILE_SCOPE_N("Raynium.Scene.RefreshBatches");
                for(const auto& [id,primitive]:state.primitives) if(!touchedPrimitives.contains(id)) {
                    const auto& mesh=state.meshes.at(primitive->desc.mesh);
                    if(replacedMeshes.contains(primitive->desc.mesh) || replacedIndices.contains(mesh->indexId)) {
                        radray::MeshBatch batch;
                        if(!Batch(*primitive,state,batch)) return fail("Terrain vertex offset exceeds 32-bit range");
                        if(!_scene.UpdateMeshBatch(primitive->batchHandle,std::move(batch))) return fail("Cannot update SOLID primitive batch");
                    }
                }
            }
        }
    }
    RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes", double(state.meshes.size()));
    RADRAY_PROFILE_PLOT("Raynium.Resident.Primitives", double(state.primitives.size()));
    RADRAY_PROFILE_PLOT("Raynium.Resident.Materials", double(state.materials.size()));
    if(f.visible_count) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Scene.VisibleBatches");
        frame.visibleBatches.reserve(f.visible_count);
        for(uint32_t i=0;i<f.visible_count;++i) {
            const auto& item=f.visible[i]; const auto it=state.primitives.find(item.primitive);
            if(it==state.primitives.end() || !item.instance_count || uint64_t(item.first_instance)+item.instance_count>f.instance_count)
                return fail("Invalid visible primitive or instance range");
            frame.visibleBatches.push_back({.Batch=it->second->batchHandle,.ElementIndex=item.submesh,.VisibilitySortKey=item.sort_key,
                .FirstInstanceOverride=item.first_instance,.InstanceCountOverride=item.instance_count});
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.StageInstances");
        if(f.instance_count) {
            const uint64_t size=uint64_t(f.instance_count)*sizeof(rn_instance);
            if(!slot.instances.Reserve(device,size,error)) return false;
            std::memcpy(slot.instances.Data(),f.instances,size_t(size));
            slot.instances.Flush(0,size);
        }
    }
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Count", double(frame.uploads.size()));
    RADRAY_PROFILE_PLOT("Raynium.Arena.Blocks", double(_arena->BlockCount()));
#ifdef RADRAY_ENABLE_PROFILER
    uint64_t uploadBytes=0;
    for(const auto& upload:frame.uploads) uploadBytes+=upload.bytes;
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Bytes", double(uploadBytes));
    RADRAY_PROFILE_PLOT("Raynium.Arena.FreeBytes", double(_arena->FreeBytes()));
    RADRAY_PROFILE_PLOT("Raynium.Frame.StagingCapacity", double(slot.staging.Capacity()));
    RADRAY_PROFILE_PLOT("Raynium.Frame.InstanceCapacity", double(slot.instances.Capacity()));
#endif
    RADRAY_PROFILE_PLOT("Raynium.RetiredResources", double(_retiredResources.size()));
    frame.retiredResources.swap(_retiredResources);
    return true;
}
void TerrainScene::DetachTextures() noexcept {
    _scene.Clear(); _state.materialBindingsByHandle.clear();
    _retiredResources.clear();
    for(auto& [id,material]:_state.materials) { material->binding.reset(); material->handle={}; }
    for(auto& [id,primitive]:_state.primitives) { primitive->primitiveHandle={}; primitive->batchHandle={}; }
    _state.boundTextures=nullptr; _rebuildScene=true;
}
void TerrainScene::Reset() noexcept { _scene.Clear(); _state=TerrainState{}; _retiredResources.clear(); _rebuildScene=true; }
uint64_t TerrainScene::ResidentBytes() const noexcept {
    uint64_t bytes=0;
    for(const auto& [id,index]:_state.indices) bytes+=index->buffer->GetDesc().Size;
    for(const auto& [id,mesh]:_state.meshes) bytes+=mesh->geometry.ByteSize();
    return bytes;
}
}
