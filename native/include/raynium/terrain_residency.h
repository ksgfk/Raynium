#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/mesh_pass.h>
#include <raynium/terrain_draw.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/upload_buffer.h>

namespace raynium {
struct TerrainMesh {
    TerrainMeshLayout layout;
    radray::VertexBufferSubAllocation geometry;
    radray::shared_ptr<radray::IndexBufferSubAllocation> indices;
    radray::shared_ptr<const TerrainDrawTemplate> draws;
    uint32_t stateId{};
};
struct TerrainState {
    uint64_t epoch{},nextMaterialKey{};
    radray::unordered_map<uint64_t,radray::shared_ptr<TerrainMesh>> meshes;
    radray::shared_ptr<radray::IndexBufferSubAllocation> quadIndices;
    uint32_t quadVertexCapacity{};
    radray::shared_ptr<MeshMaterialBinding> material;
    uint32_t albedo{},light{},anisotropy{};
    TerrainBindingStates bindings{};
};
struct SlotUploads {
    radray::FrameSlot& frame;
    radray::BatchedBufferUpload& staging;
    radray::MappedUploadBuffer& instances;
    radray::MappedUploadBuffer& indirect;
    TerrainFrameLists& lists;
    bool useIndirect{};
    radray::MappedUploadBuffer& textureStaging;
    radray::vector<TextureCopy>& textureCopies;
};
// ABI arrays are copied during PrepareFrame. Lists remain read-only through RunFrame.
struct TerrainFrame {
    const TerrainFrameLists* lists{};
    radray::Nullable<const radray::VertexBufferPool*> vertices{nullptr};
    radray::Nullable<const radray::IndexBufferPool*> indices{nullptr};
    rn_render_frame_desc parameters{};
    bool hasTranslucent{};
    radray::render::Texture* color{};
    radray::render::SwapChainSyncObject* waitToDraw{};
    radray::render::SwapChainSyncObject* readyToPresent{};
    radray::render::Buffer* textureStaging{};
    const radray::vector<TextureCopy>* textureCopies{};
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
