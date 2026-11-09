#pragma once
#include <raynium/mesh_pass.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/upload_buffer.h>
#include <unordered_map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace raynium {
struct TerrainMesh {
    uint64_t indexId{};
    uint32_t vertexCount{};
    std::vector<rn_submesh> ranges;
    radray::VertexBufferSubAllocation geometry; // Released after the retirement boundary.
};
struct TerrainMaterial {
    rn_material_update desc{};
    radray::MaterialHandle handle{};
    std::shared_ptr<MeshMaterialBinding> binding;
};
struct TerrainPrimitive {
    rn_primitive_update desc{};
    radray::PrimitiveHandle primitiveHandle{};
    radray::MeshBatchHandle batchHandle{};
    uint32_t elementCount{};
};
struct TerrainState {
    uint64_t epoch{};
    const SharedTextures* boundTextures{};
    std::unordered_map<uint64_t,std::shared_ptr<radray::IndexBufferSubAllocation>> indices;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMesh>> meshes;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMaterial>> materials;
    std::unordered_map<uint64_t,TerrainPrimitive> primitives;
    MaterialBindingMap materialBindingsByHandle;
};
struct TerrainFrameLists {
    std::vector<radray::VisibleMeshBatch> visibleBatches;
};
struct SlotUploads {
    radray::FrameSlot& frame;
    radray::BatchedBufferUpload& staging;
    radray::MappedUploadBuffer& instances;
    TerrainFrameLists& lists;
};
// Lists borrow slot storage through RunFrame; Wait and destruction never read them. ABI pointers end at PrepareFrame.
struct TerrainFrame {
    std::span<const radray::VisibleMeshBatch> visibleBatches;
    radray::Nullable<const radray::VertexBufferPool*> vertices{nullptr};
    radray::Nullable<const radray::IndexBufferPool*> indices{nullptr};
    rn_render_frame_desc parameters{};
    RenderTarget* target{};
    SharedTextures* textures{};
};
class TerrainScene final {
public:
    TerrainScene(RayniumRuntime& device,radray::unique_ptr<radray::VertexBufferPool> vertices,
        radray::unique_ptr<radray::IndexBufferPool> indices);
    bool PrepareFrame(RenderTarget&,SharedTextures*,const rn_render_frame_desc&,const MeshPassResources&,
        SlotUploads,TerrainFrame&,radray::string&);
    // Requires WaitIdle before releasing bindings borrowed by cached commands or recordings.
    void DetachTextures() noexcept;
    void Reset() noexcept;
    radray::Scene& Scene() noexcept { return _scene; }
    const MaterialBindingMap& MaterialBindings() const noexcept { return _state.materialBindingsByHandle; }
    uint64_t ResidentBytes() const noexcept;
private:
    RayniumRuntime& _device;
    // Declared before the resident state so every geometry allocation is released first.
    radray::unique_ptr<radray::VertexBufferPool> _vertices;
    radray::unique_ptr<radray::IndexBufferPool> _indices;
    TerrainState _state;
    radray::Scene _scene;
    bool _rebuildScene{true};
};
}
