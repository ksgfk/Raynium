#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/mesh_pass.h>
#include <raynium/terrain_draw.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/geometry_buffer_pool.h>

namespace raynium {
struct TerrainMesh {
    TerrainMeshLayout layout;
    radray::VertexBufferSubAllocation geometry;
    radray::IndexBufferSubAllocation indices;
    TerrainDrawTemplate draws;
    uint32_t stateId{};
};
struct TerrainState {
    uint64_t epoch{},nextMaterialKey{};
    radray::unordered_map<uint64_t,TerrainMesh> meshes;
    radray::IndexBufferSubAllocation quadIndices;
    uint32_t quadVertexCapacity{};
    radray::unique_ptr<MeshMaterialBinding> material;
    uint32_t albedo{},light{},anisotropy{};
    TerrainBindingStates bindings{};
};
struct GeometryUpload {
    radray::IntrusivePtr<radray::GpuBuffer> target;
    uint64_t offset{};
    radray::RDGUploadSource source;
};
struct SlotUploads {
    radray::FrameSlot& frame;
    radray::vector<GeometryUpload>& geometry;
    TerrainFrameLists& lists;
    bool useIndirect{};
};
// ABI payloads are borrowed until synchronous RDG Prepare; no borrow is used by Wait or cleanup.
struct TerrainFrame {
    const TerrainFrameLists* lists{};
    radray::vector<GeometryUpload>* geometry{};
    std::span<const rn_instance> instances;
    UploadedTextures* textures{};
    rn_render_frame_desc parameters{};
    bool hasTranslucent{};
    radray::render::Texture* color{};
    radray::render::SwapChainSyncObject* waitToDraw{};
    radray::render::SwapChainSyncObject* readyToPresent{};
};
class TerrainResidency final {
public:
    TerrainResidency(radray::DeviceContext&,radray::unique_ptr<radray::VertexBufferPool>,radray::unique_ptr<radray::IndexBufferPool>);
    bool PrepareFrame(const UploadedTextures&,const rn_render_frame_desc&,const MeshPassResources&,SlotUploads,TerrainFrame&);
    void Reset() noexcept;
    uint64_t ResidentBytes() const noexcept;
private:
    radray::DeviceContext& _device;
    radray::unique_ptr<radray::VertexBufferPool> _vertices;
    radray::unique_ptr<radray::IndexBufferPool> _indices;
    TerrainState _state;
    radray::unordered_set<radray::render::Buffer*> _vertexPages,_indexPages;
    uint64_t _vertexReservedBytes{},_indexReservedBytes{};
};
}
