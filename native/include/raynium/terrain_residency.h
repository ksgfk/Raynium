#pragma once
#include <raynium/mesh_pass.h>
#include <raynium/terrain_draw.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/upload_buffer.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace raynium {
struct TerrainMesh {
    uint64_t indexId{};
    uint32_t vertexCount{};
    std::vector<rn_submesh> ranges;
    radray::VertexBufferSubAllocation geometry;
    std::shared_ptr<const TerrainDrawTemplate> draws;
};
struct TerrainMaterial { rn_material_update desc{}; std::shared_ptr<MeshMaterialBinding> binding; };
struct TerrainPrimitive {
    rn_primitive_update desc{};
    std::shared_ptr<const TerrainDrawTemplate> draws;
    std::shared_ptr<const MeshMaterialBinding> binding;
    uint32_t fullMask{};
    uint32_t stateSortKey{};
    rn_material_mode renderMode{};
};
struct TerrainState {
    uint64_t epoch{},nextMaterialKey{};
    const SharedTextures* boundTextures{};
    std::unordered_map<uint64_t,std::shared_ptr<radray::IndexBufferSubAllocation>> indices;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMesh>> meshes;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMaterial>> materials;
    std::unordered_map<uint64_t,TerrainPrimitive> primitives;
    std::unordered_map<uint64_t,std::unordered_set<uint64_t>> meshesByIndex,primitivesByMesh,primitivesByMaterial;
};
struct SlotUploads {
    radray::FrameSlot& frame;
    radray::BatchedBufferUpload& staging;
    radray::MappedUploadBuffer& instances;
    radray::MappedUploadBuffer& indirect;
    TerrainFrameLists& lists;
    bool useIndirect{};
};
// ABI arrays are copied during PrepareFrame. Lists remain read-only through RunFrame.
struct TerrainFrame {
    const TerrainFrameLists* lists{};
    radray::Nullable<const radray::VertexBufferPool*> vertices{nullptr};
    radray::Nullable<const radray::IndexBufferPool*> indices{nullptr};
    rn_render_frame_desc parameters{};
    bool hasTranslucent{};
    RenderTarget* target{};
    SharedTextures* textures{};
};
class TerrainResidency final {
public:
    TerrainResidency(RayniumRuntime&,radray::unique_ptr<radray::VertexBufferPool>,radray::unique_ptr<radray::IndexBufferPool>);
    bool PrepareFrame(RenderTarget&,SharedTextures*,const rn_render_frame_desc&,const MeshPassResources&,SlotUploads,TerrainFrame&,radray::string&);
    // Requires WaitIdle before releasing texture bindings.
    void DetachTextures() noexcept;
    void Reset() noexcept;
    uint64_t ResidentBytes() const noexcept;
private:
    RayniumRuntime& _device;
    radray::unique_ptr<radray::VertexBufferPool> _vertices;
    radray::unique_ptr<radray::IndexBufferPool> _indices;
    TerrainState _state;
};
}
