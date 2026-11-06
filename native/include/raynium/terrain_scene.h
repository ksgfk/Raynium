#pragma once
#include <raynium/geometry_arena.h>
#include <raynium/mesh_pass.h>
#include <radray/runtime/frame_scheduler.h>
#include <unordered_map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace raynium {
struct TerrainIndex {
    RenderBuffer buffer;
};
struct TerrainMesh {
    uint64_t indexId{};
    uint32_t vertexCount{};
    std::vector<rn_submesh> ranges;
    GeometryAllocation geometry; // Range load may still be in flight; the retirement path frees it.
};
struct TerrainMaterial {
    rn_material_update desc{};
    radray::SparseSetHandle handle{};
    std::shared_ptr<MeshMaterialBinding> binding;
};
struct TerrainPrimitive {
    rn_primitive_update desc{};
    radray::SparseSetHandle primitiveHandle{},batchHandle{};
    radray::render::Buffer* vertexBuffer{};
    radray::render::Buffer* indexBuffer{};
    uint32_t elementCount{};
};
struct TerrainState {
    uint64_t epoch{};
    const SharedTextures* boundTextures{};
    std::unordered_map<uint64_t,std::shared_ptr<TerrainIndex>> indices;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMesh>> meshes;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMaterial>> materials;
    std::unordered_map<uint64_t,TerrainPrimitive> primitives;
    MaterialBindingMap materialBindingsByHandle;
};
// One copy recorded per frame; staging offsets index the frame slot's mapped staging buffer.
struct GeometryUpload {
    radray::render::Buffer* destination;
    uint64_t destinationOffset,stagingOffset,bytes;
};
using GeometryAccess=std::pair<radray::render::Buffer*,radray::render::BufferState>;
struct TerrainFrameLists {
    std::vector<radray::VisibleMeshBatch> visibleBatches;
    std::vector<GeometryAccess> geometry;
};
struct SlotUploads {
    MappedUploadBuffer& staging;
    MappedUploadBuffer& instances;
    TerrainFrameLists& lists;
};
// Lists borrow slot storage through RunFrame; Wait and destruction never read them. ABI pointers end at PrepareFrame.
struct TerrainFrame {
    std::vector<GeometryUpload> uploads;
    std::span<const radray::VisibleMeshBatch> visibleBatches;
    std::span<const GeometryAccess> geometry;
    std::vector<std::shared_ptr<void>> retiredResources;
    rn_render_frame_desc parameters{};
    RenderTarget* target{};
    SharedTextures* textures{};
};
class TerrainScene final {
public:
    TerrainScene(RayniumRuntime& device,radray::unique_ptr<GeometryArena> arena);
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
    radray::unique_ptr<GeometryArena> _arena;
    TerrainState _state;
    radray::Scene _scene;
    // Failed preparation keeps retired owners alive until Reset after WaitIdle.
    std::vector<std::shared_ptr<void>> _retiredResources;
    bool _rebuildScene{true};
};
}
