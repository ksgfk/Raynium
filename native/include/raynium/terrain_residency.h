#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <type_traits>

#include <radray/gpu/types.h>
#include <radray/inline_vector.h>
#include <radray/nullable.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/types.h>
#include <raynium/mesh_pass.h>
#include <raynium/terrain_draw.h>

namespace raynium {
inline constexpr uint8_t kTerrainHotFlagActive = 1u << 0;
inline constexpr uint8_t kTerrainHotFlagFiniteBounds = 1u << 1;
inline constexpr uint8_t kTerrainHotFlagWideCounts = 1u << 2;
inline constexpr uint8_t kTerrainHotFlagHasIndices = 1u << 3;

struct alignas(64) TerrainHotSlot {
    float MinBound[3]{};
    float MaxBound[3]{};
    uint32_t FirstVertex{};
    uint32_t FirstIndex{};
    uint32_t IndexCount{};
    uint16_t VertexPageId{};
    uint16_t IndexPageId{};
    uint8_t Layer{};
    uint8_t FullMask{};
    uint8_t ValidMask{};
    uint8_t Flags{};
    uint8_t GroupBits[7]{};
    uint8_t SectionBoundsAxes{};
    // End quad of each packed range; the seventh range ends at IndexCount.
    uint16_t RangeEndQuads[6]{};
};
static_assert(sizeof(TerrainHotSlot) == 64 && alignof(TerrainHotSlot) == 64);

struct ProjectedTerrainBounds {
    float Left{};
    float Right{};
    float Bottom{};
    float Top{};
    bool Valid{};
};

bool AreProjectedBoundsDisjoint(const ProjectedTerrainBounds& a, const ProjectedTerrainBounds& b) noexcept;
ProjectedTerrainBounds ProjectTerrainBounds(
    const TerrainHotSlot& hot,
    const rn_instance& instance,
    const rn_terrain_view_desc& frame) noexcept;

class TerrainEyeCrossingCache final {
public:
    struct Item {
        uint32_t Mesh{};
        uint32_t FirstInstance{};
        uint32_t InstanceCount{};
        uint8_t RequestedMask{};
        uint8_t EffectiveMask{};
    };
    static_assert(sizeof(Item) == 16);

    radray::array<int32_t, 3> BaseBlock{};
    radray::array<double, 3> Center{};
    radray::vector<Item> CrossingItems;
    bool EyeValid{};
    radray::array<double, 3> Lower{-1, -1, -1};
    radray::array<double, 3> Upper{1, 1, 1};
    radray::array<double, 3> LastDelta{};

    void Reset(const radray::array<int32_t, 3>& block, const radray::array<double, 3>& eye, bool valid) noexcept;
    uint32_t Capture(const rn_visible_item& item, const TerrainHotSlot& hot, std::span<const rn_instance> instances);
    void IncludePlane(uint32_t axis, double delta) noexcept;
    bool Matches(
        const radray::array<int32_t, 3>& block,
        const radray::array<double, 3>& eye,
        bool valid,
        std::span<const TerrainHotSlot> hotSlots,
        std::span<const rn_instance> instances) noexcept;
};

uint32_t ComputeHotSlotVisibleMask(
    const TerrainHotSlot& hot,
    const radray::array<int32_t, 3>& block,
    const radray::array<double, 3>& eye,
    bool valid,
    std::span<const rn_instance> instances,
    uint32_t requested = UINT32_MAX) noexcept;

// Emits merged (instanceCount==1) or unmerged (instanceCount>1) MDI records directly from a TerrainHotSlot.
void EmitHotSlotSolidIndirectArgs(
    const TerrainHotSlot& hot,
    const TerrainMeshLayout* wideFallbackLayout,
    uint32_t quadBaseFirstIndex,
    uint32_t mask,
    uint32_t firstInstance,
    uint32_t instanceCount,
    radray::vector<radray::gpu::DrawIndexedIndirectArguments>& outArgs);

struct TerrainWireframe {
    uint32_t Mask{};
    radray::IndexBufferSubAllocation Indices;
};

struct TerrainMesh {
    TerrainMeshLayout Layout;
    radray::VertexBufferSubAllocation Geometry;
    radray::IndexBufferSubAllocation Indices;
    uint32_t FullMask{};
    uint32_t ValidMask{};
    uint32_t VertexPageId{};
    uint64_t DirtyStamp{};
    bool Active{};
    radray::vector<TerrainWireframe> Wireframes;
};
static_assert(std::is_nothrow_destructible_v<TerrainMesh>);

struct GeometryUpload {
    radray::IntrusivePtr<radray::GpuBuffer> Target;
    uint64_t Offset{};
    radray::RDGUploadSource Source;
    // When present, the packed source fills these destination ranges in order.
    std::span<const radray::gpu::BufferRange> CopyRanges{};
};

void ImportGeometryUpload(GeometryUpload& upload, radray::RDGBuilder& graph);

class TerrainLayerDrawData final {
public:
    radray::PreparedMeshPassIndirectDraws Prepared;
    radray::vector<radray::VisibleMeshDrawCommand> IndirectReps;
    // Per-layer representative commands for the direct MDI fast path (owned independently per layer).
    radray::vector<radray::MeshDrawCommand> FastCommands;
    radray::MeshDrawCommandBindingStorage FastBindings;

    uint64_t IndirectSize{};
    uint32_t RecordCount{};
    uint64_t SelectedGroups{};
    uint64_t IndicesInstances{};

    void Clear() noexcept;
};

class TerrainViewDrawData final {
public:
    radray::array<TerrainLayerDrawData, 3> Layers;
    // OIT 3 stages share Layers[2].Prepared and a single RDG IndirectArgs slice;
    // only the B representative VisibleMeshDrawCommands (typically B <= 4) are mapped per stage.
    radray::array<radray::vector<radray::VisibleMeshDrawCommand>, 3> OitIndirectReps;
    bool Oit{};
    bool Wireframe{};
    bool HasAnyDraws{};
    uint32_t TotalIndirectArguments{};

    void Clear() noexcept;
};

class SlotPromotedBuffer final {
public:
    radray::IntrusivePtr<radray::GpuBuffer> Buffer;
    uint64_t LastBytes{};
    uint32_t StableFrames{};

    // Failure preserves the cache and both output arguments.
    bool Prepare(
        radray::gpu::Device& device,
        bool unchanged,
        uint64_t bytes,
        radray::gpu::BufferUses usage,
        radray::IntrusivePtr<radray::GpuBuffer>& output,
        bool& upload,
        bool hasDraws = true);
};

class TerrainViewCacheSlot final {
public:
    struct Layer {
        uint64_t GeometryVersion{};
        TerrainEyeCrossingCache Crossing;
        size_t VisibleCount{};
        bool Valid{};
    };

    TerrainViewDrawData Draws;
    radray::array<Layer, 3> Layers;
    SlotPromotedBuffer InstanceBuffer;
    radray::IntrusivePtr<radray::GpuBuffer> InstancePool;
    bool PooledInstances{};
    radray::array<SlotPromotedBuffer, 3> ArgumentBuffers;
    radray::vector<rn_visible_item> Visible;
    radray::vector<rn_instance> Instances;
    radray::array<radray::vector<uint32_t>, 3> LayerItems;
    uint64_t MaxInstanceEnd{};
    uint64_t Epoch{};
    uint64_t PartitionVersion{};
    uint64_t VisibleVersion{};
    uint64_t OriginsVersion{};
    uint64_t InstancesVersion{};

    bool MatchesVisible(const rn_terrain_view_desc& view) const noexcept;
    bool MatchesOrigins(const rn_terrain_view_desc& view) const noexcept;
    bool MatchesInstances(const rn_terrain_view_desc& view) const noexcept;
};

struct SlotUploads {
    radray::FrameSlot& Frame;
    std::pmr::vector<GeometryUpload>& Geometry;
    TerrainViewCacheSlot& Cache;
    bool UseIndirect{};
};

// ABI payloads are borrowed until synchronous RDG Prepare; no borrow is used by Wait or cleanup.
struct TerrainFrame {
    const TerrainViewDrawData* Draws{};
    std::pmr::vector<GeometryUpload>* Geometry{};
    std::span<const rn_instance> Instances;
    radray::IntrusivePtr<radray::GpuBuffer> PersistentInstances;
    radray::array<radray::IntrusivePtr<radray::GpuBuffer>, 3> PersistentIndirect;
    radray::array<bool, 3> UploadIndirect{};
    bool UploadInstances{};
    PendingResourceOps* Uploads{};
    std::span<const ShadowWrite> ShadowWrites;
    rn_terrain_view_desc Parameters{};
    bool HasTranslucent{};
    uint32_t Width{};
    uint32_t Height{};
    radray::Nullable<const radray::gpu::SwapChainFrame*> SurfaceFrame{nullptr};
};

class TerrainResidency final {
public:
    static constexpr uint32_t kMaxMeshSlot = 1048576u;

    TerrainResidency(
        radray::DeviceContext& device,
        radray::unique_ptr<radray::VertexBufferPool> vertices,
        radray::unique_ptr<radray::VertexBufferPool> translucentVertices,
        radray::unique_ptr<radray::IndexBufferPool> quadPool,
        radray::unique_ptr<radray::IndexBufferPool> indices);
    void BeginSlotViews(std::span<const rn_terrain_view_desc> views, radray::FrameSlot& slot);
    TerrainViewCacheSlot& View(size_t index);
    bool PrepareFrame(
        rn_renderer* owner,
        const rn_terrain_view_desc& viewDesc,
        const MeshPassResources& resources,
        SlotUploads slot,
        TerrainFrame& frame);
    void Reset() noexcept;
    uint64_t ResidentBytes() const noexcept;
    void MeasureGeometry(rn_frame_metrics& metrics) const noexcept;
    const radray::VertexBufferPool& VertexPool() const noexcept;
    const radray::VertexBufferPool& TranslucentVertexPool() const noexcept;
    const radray::IndexBufferPool& QuadIndexPool() const noexcept;
    const radray::IndexBufferPool& TranslucentIndexPool() const noexcept;
    const radray::IndexBufferSubAllocation& QuadIndices() const noexcept;

    bool EnsureQuadIndices(
        uint32_t vertexCount,
        std::pmr::vector<GeometryUpload>& uploads,
        radray::FrameSlot* retireSlot,
        uint64_t* uploadBytesOut = nullptr,
        bool* grewOut = nullptr);
    bool ResyncQuadMeshes();

private:
    struct RetiredTerrainAllocations {
        radray::vector<radray::VertexBufferSubAllocation> Vertices;
        radray::vector<radray::IndexBufferSubAllocation> Indices;
    };

    struct TerrainState {
        uint64_t Epoch{};
        radray::vector<TerrainMesh> MeshesBySlot;
        uint32_t ResidentMeshCount{};
        bool WireframeActive{};
    };

    struct FastPageBucket {
        radray::vector<radray::gpu::DrawIndexedIndirectArguments> Args;
        uint32_t RepSlotId{};
        uint32_t WireMask{};
        bool HasNonZeroFirstInstance{};
    };

    struct FastOitBucket {
        uint16_t VertexPageId{};
        uint16_t IndexPageId{};
        uint32_t RepSlotId{};
        bool HasNonZeroFirstInstance{};
        radray::vector<radray::gpu::DrawIndexedIndirectArguments> Args;
    };

    struct FastClassicTranslucentItem {
        uint64_t SortKey{};
        uint32_t RawSortKey{};
        uint32_t SlotId{};
        uint16_t VertexPageId{};
        uint16_t IndexPageId{};
        radray::gpu::DrawIndexedIndirectArguments Arg{};
    };

    RetiredTerrainAllocations& Retired(radray::FrameSlot& slot);
    void RetireMesh(TerrainMesh& mesh, radray::FrameSlot& slot);
    bool ValidateAndSyncHotSlot(uint32_t slotId);
    bool GroupDisjointClassicItems(std::span<const rn_instance> instances, const rn_terrain_view_desc& frame);

    RetiredTerrainAllocations* _retired{};
    radray::DeviceContext& _device;
    radray::unique_ptr<radray::VertexBufferPool> _vertices;
    radray::unique_ptr<radray::VertexBufferPool> _translucentVertices;
    radray::unique_ptr<radray::IndexBufferPool> _quadPool;
    radray::unique_ptr<radray::IndexBufferPool> _indices;
    radray::IndexBufferSubAllocation _quadIndices;
    uint32_t _quadVertexCapacity{};
    TerrainState _state;
    radray::vector<TerrainHotSlot> _hotSlots;
    radray::vector<uint32_t> _changedSlotsScratch;
    uint64_t _syncStamp{};
    radray::vector<radray::unique_ptr<TerrainViewCacheSlot>> _views;
    uint64_t _partitionVersion{1};
    radray::array<uint64_t, 3> _layerGeometryVersion{1, 1, 1};
    radray::array<radray::vector<FastPageBucket>, 2> _fastPageBuckets;
    radray::vector<FastOitBucket> _fastOitBuckets;
    radray::unordered_map<uint32_t, size_t> _oitPageLookup;
    radray::array<radray::unordered_map<uint64_t, uint32_t>, 2> _wirePages;
    radray::vector<FastClassicTranslucentItem> _fastClassicTranslucent;
    radray::vector<ProjectedTerrainBounds> _classicBounds;
    radray::vector<uint32_t> _fastBatchRepSlots;
};
}  // namespace raynium
