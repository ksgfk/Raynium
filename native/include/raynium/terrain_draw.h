#pragma once
#include <raynium/bridge.h>
#include <radray/render/rhi.h>
#include <array>
#include <span>
#include <unordered_map>
#include <vector>

namespace raynium {
struct MeshMaterialBinding;
struct TerrainDrawRange { uint32_t first{},count{}; int32_t vertex{}; };
struct TerrainDrawTemplate {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    std::vector<TerrainDrawRange> ranges;
    uint32_t fullMask{},validMask{},mergeMask{};
};
struct TerrainBindingState {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    const MeshMaterialBinding* material{};
};
class TerrainBindingStates final {
public:
    explicit TerrainBindingStates(uint32_t limit=UINT32_MAX):_limit(limit),_states(1),_entries(1) {}
    bool Acquire(const TerrainDrawTemplate&,const MeshMaterialBinding&,uint32_t& id,radray::string& error);
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
    std::unordered_map<Key,uint32_t,Hash> _lookup;
    std::vector<TerrainBindingState> _states;
    std::vector<Entry> _entries;
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
};
struct TerrainDrawLayer { uint32_t firstBatch{},batchCount{},firstRecord{},recordCount{}; uint64_t selectedGroups{},indicesInstances{}; };
struct TerrainFrameLists {
    std::array<std::vector<TerrainVisible>,3> visible;
    std::array<std::vector<TerrainSortItem>,3> order;
    std::vector<TerrainSortItem> sortScratch;
    std::vector<radray::render::DrawIndexedIndirectArguments> arguments;
    std::vector<TerrainDrawBatch> batches;
    std::array<TerrainDrawLayer,3> layers;
};
// Inputs retain their relative order for equal state/depth keys. Multi-instance items never merge face groups.
bool BuildTerrainDraws(TerrainFrameLists&,std::span<const TerrainBindingState>,bool oit,radray::string& error);
// Returns the next native-call record count; zero means unsupported or no remaining records.
uint32_t TerrainIndirectChunk(uint32_t remaining,uint32_t nativeLimit) noexcept;
// Successful auto fallback returns its reason; forced unsupported modes fail before resource creation.
bool SelectTerrainSubmission(uint32_t flags,const radray::render::RenderDeviceCapabilities&,bool& indirect,radray::string& reason);
}
