#pragma once
#include <radray/types.h>
#include <raynium/bridge.h>
#include <radray/render/rhi.h>
#include <span>
#include <memory_resource>
#include <radray/runtime/gpu_resource.h>

namespace raynium {
struct MeshMaterialBinding;
struct TerrainDrawRange { uint32_t first{},count{}; int32_t vertex{}; };
struct TerrainMeshLayout {
    radray::vector<TerrainDrawRange> ranges;
    radray::array<uint32_t,7> groupBits{};
    radray::array<double,3> minimum{},maximum{};
    uint32_t vertexCount{};
    rn_material_mode layer{};
    bool finiteBounds{};
};
struct TerrainEye { radray::array<double,3> offset{}; bool valid{}; };
struct TerrainMeshUpload {
    explicit TerrainMeshUpload(std::pmr::memory_resource& memory):groups(&memory) {}
    std::span<const radray::byte> source;
    std::pmr::vector<uint8_t> groups;
    radray::array<uint32_t,7> offsets{};
    bool Write(std::span<radray::byte>) const noexcept;
};
bool AnalyzeTerrainMesh(std::span<const radray::byte>,rn_material_mode,TerrainMeshLayout&,TerrainMeshUpload&);
bool WriteQuadIndices(std::span<radray::byte>,uint32_t vertexCount) noexcept;
TerrainEye ComputeTerrainEye(const rn_render_frame_desc&) noexcept;
uint32_t TerrainVisibleMask(const TerrainMeshLayout&,const TerrainEye&,const rn_render_frame_desc&,
                            std::span<const rn_instance>) noexcept;
struct TerrainDrawTemplate {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    radray::vector<TerrainDrawRange> ranges;
    uint32_t fullMask{},validMask{},mergeMask{};
    radray::GpuBuffer* vertexPage{};
    radray::GpuBuffer* indexPage{};
};
struct TerrainBindingState {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    const MeshMaterialBinding* material{};
    radray::GpuBuffer* vertexPage{};
    radray::GpuBuffer* indexPage{};
};
class TerrainBindingStates final {
public:
    explicit TerrainBindingStates(uint32_t limit=UINT32_MAX):_limit(limit),_states(1),_entries(1) {}
    bool Acquire(const TerrainDrawTemplate&,const MeshMaterialBinding&,uint32_t& id);
    // IDs are CPU preparation handles. Recorded batches copy views and borrow separately retired owners.
    void Release(uint32_t id) noexcept;
    std::span<const TerrainBindingState> States() const noexcept { return _states; }
    uint32_t Count() const noexcept { return uint32_t(_lookup.size()); }
private:
    struct Key {
        radray::render::VertexBufferView vertices{};
        radray::render::IndexBufferView indices{};
        const radray::render::ShaderParameterSet* parameters{};
        uint64_t materialVersion{};
        bool operator==(const Key&) const noexcept;
    };
    struct Hash { size_t operator()(const Key&) const noexcept; };
    struct Entry { Key key{}; uint32_t references{},nextFree{}; };
    uint32_t _limit{},_freeHead{};
    radray::unordered_map<Key,uint32_t,Hash> _lookup;
    radray::vector<TerrainBindingState> _states;
    radray::vector<Entry> _entries;
};
struct TerrainVisible {
    const TerrainDrawRange* ranges{};
    uint32_t mask{},mergeMask{},firstInstance{},instanceCount{},stateId{};
};
struct TerrainSortItem {
    // State in the high 32 bits, visibility/depth key in the low 32 bits.
    uint64_t sortKey{};
    uint32_t payloadIndex{};
};
static_assert(sizeof(TerrainVisible)==32 && sizeof(TerrainSortItem)==16);
struct TerrainDrawBatch {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    const MeshMaterialBinding* material{};
    uint32_t first{},count{};
    radray::GpuBuffer* vertexPage{};
    radray::GpuBuffer* indexPage{};
};
struct TerrainDrawLayer { uint32_t firstBatch{},batchCount{},firstRecord{},recordCount{}; uint64_t selectedGroups{},indicesInstances{}; };
struct TerrainFrameLists {
    radray::array<radray::vector<TerrainVisible>,3> visible;
    radray::array<radray::vector<TerrainSortItem>,3> order;
    radray::vector<TerrainSortItem> sortScratch;
    radray::vector<radray::render::DrawIndexedIndirectArguments> arguments;
    radray::vector<TerrainDrawBatch> batches;
    radray::array<TerrainDrawLayer,3> layers;
};
// Inputs retain their relative order for equal state/depth keys. Multi-instance items never merge face groups.
bool BuildTerrainDraws(TerrainFrameLists&,std::span<const TerrainBindingState>,bool oit);
// Returns the next native-call record count; zero means unsupported or no remaining records.
uint32_t TerrainIndirectChunk(uint32_t remaining,uint32_t nativeLimit) noexcept;
// Logs the selected mode and any auto fallback; forced unsupported modes fail before resource creation.
bool SelectTerrainSubmission(uint32_t flags,const radray::render::RenderDeviceCapabilities&,bool& indirect);
}
