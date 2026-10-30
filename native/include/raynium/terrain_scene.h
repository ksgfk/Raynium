#pragma once
#include <raynium/mesh_pass.h>
#include <radray/runtime/frame_scheduler.h>
#include <unordered_map>
#include <memory>
#include <vector>

namespace raynium {
struct TerrainIndex {
    RenderBuffer buffer;
};
struct TerrainMesh {
    uint64_t indexId{};
    uint32_t vertexCount{};
    std::vector<rn_submesh> ranges;
    RenderBuffer buffer;
    radray::VertexInput input;
};
struct TerrainMaterial {
    rn_material_update desc{};
    radray::SparseSetHandle handle{};
    std::shared_ptr<MeshMaterialBinding> binding;
};
struct TerrainPrimitive {
    rn_primitive_update desc{};
    radray::SparseSetHandle primitiveHandle{},batchHandle{};
};
struct TerrainState {
    uint64_t epoch{};
    const SharedTextures* boundTextures{};
    std::unordered_map<uint64_t,std::shared_ptr<TerrainIndex>> indices;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMesh>> meshes;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainMaterial>> materials;
    std::unordered_map<uint64_t,std::shared_ptr<TerrainPrimitive>> primitives;
    MaterialBindingMap materialBindingsByHandle;
};
struct GeometryUpload {
    RenderBuffer staging;
    radray::render::Buffer* destination;
    uint64_t bytes;
    radray::render::BufferStates after;
};
// Frame resources survive until the slot allocator resets; ABI pointers never survive PrepareFrame.
struct TerrainFrame {
    std::vector<GeometryUpload> uploads;
    std::vector<radray::VisibleMeshBatch> visibleBatches;
    std::vector<std::shared_ptr<void>> retiredResources;
    RenderBuffer instances,timing;
    radray::unique_ptr<radray::render::Texture> depth;
    radray::unique_ptr<radray::render::TextureView> colorView,depthView;
    radray::unique_ptr<radray::render::QueryPool> timestamps;
    rn_render_frame_desc parameters{};
    RenderTarget* target{};
    SharedTextures* textures{};
};
class TerrainScene final {
public:
    explicit TerrainScene(RayniumRuntime& device):_device(device) {}
    bool PrepareFrame(RenderTarget&,SharedTextures*,const rn_render_frame_desc&,const MeshPassResources&,
        TerrainFrame&,radray::string&);
    // Requires WaitIdle before releasing bindings borrowed by cached commands or recordings.
    void DetachTextures() noexcept;
    void Reset() noexcept;
    radray::Scene& Scene() noexcept { return _scene; }
    const MaterialBindingMap& MaterialBindings() const noexcept { return _state.materialBindingsByHandle; }
    uint64_t ResidentBytes() const noexcept;
private:
    RayniumRuntime& _device;
    TerrainState _state;
    radray::Scene _scene;
    // Failed preparation keeps retired owners alive until Reset after WaitIdle.
    std::vector<std::shared_ptr<void>> _retiredResources;
    bool _rebuildScene{true};
};
}
