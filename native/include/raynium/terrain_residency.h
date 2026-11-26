#pragma once
#include <radray/types.h>
#include <radray/inline_vector.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <raynium/mesh_pass.h>
#include <raynium/terrain_draw.h>
#include <cmath>
#include <limits>

namespace raynium {
inline constexpr uint8_t kTerrainHotFlagActive = 1u << 0;
inline constexpr uint8_t kTerrainHotFlagFiniteBounds = 1u << 1;
inline constexpr uint8_t kTerrainHotFlagWideCounts = 1u << 2;
inline constexpr uint8_t kTerrainHotFlagHasIndices = 1u << 3;

struct alignas(64) TerrainHotSlot {
    float minBound[3]{};
    float maxBound[3]{};
    uint32_t firstVertex{};
    uint32_t firstIndex{};
    uint32_t indexCount{};
    uint16_t vertexPageId{};
    uint16_t indexPageId{};
    uint8_t layer{};
    uint8_t fullMask{};
    uint8_t validMask{};
    uint8_t flags{};
    uint8_t groupBits[7]{};
    uint8_t sectionBoundsAxes{};
    // End quad of each packed range; the seventh range ends at indexCount.
    uint16_t rangeEndQuads[6]{};
};
static_assert(sizeof(TerrainHotSlot) == 64 && alignof(TerrainHotSlot) == 64);

struct ProjectedTerrainBounds {
    float left{},right{},bottom{},top{};
    bool valid{};
    bool Disjoint(const ProjectedTerrainBounds& other) const noexcept;
};
ProjectedTerrainBounds ProjectTerrainBounds(const TerrainHotSlot&,const rn_instance&,const rn_terrain_view_desc&) noexcept;

struct TerrainEyeCrossingCache {
    struct Item { uint32_t mesh,firstInstance,instanceCount; uint8_t requestedMask,effectiveMask; };
    static_assert(sizeof(Item)==16);
    radray::array<int32_t,3> baseBlock{};
    radray::array<double,3> center{};
    radray::vector<Item> crossingItems;
    bool eyeValid{};
    radray::array<double,3> lower{-1,-1,-1}, upper{1,1,1}, lastDelta{};
    void Reset(const radray::array<int32_t,3>& block,const radray::array<double,3>& eye,bool valid) noexcept;
    uint32_t Capture(const rn_visible_item&,const TerrainHotSlot&,std::span<const rn_instance> instances);
    void IncludePlane(uint32_t axis,double delta) noexcept;
    bool Matches(const radray::array<int32_t,3>& block,const radray::array<double,3>& eye,bool valid,
        std::span<const TerrainHotSlot> hotSlots,std::span<const rn_instance> instances) noexcept;
};

uint32_t ComputeHotSlotVisibleMask(const TerrainHotSlot& hot,const radray::array<int32_t,3>& block,
    const radray::array<double,3>& eye,bool valid,std::span<const rn_instance> instances,uint32_t requested=UINT32_MAX) noexcept;

// Emits merged (instanceCount==1) or unmerged (instanceCount>1) MDI records directly from a TerrainHotSlot.
void EmitHotSlotSolidIndirectArgs(const TerrainHotSlot& hot, const TerrainMeshLayout* wideFallbackLayout,
    uint32_t quadBaseFirstIndex, uint32_t mask, uint32_t firstInstance, uint32_t instanceCount,
    radray::vector<radray::gpu::DrawIndexedIndirectArguments>& outArgs);
struct TerrainWireframe {
    uint32_t mask{};
    radray::IndexBufferSubAllocation indices;
};
struct TerrainMesh {
    TerrainMeshLayout layout;
    radray::VertexBufferSubAllocation geometry;
    radray::IndexBufferSubAllocation indices;
    uint32_t fullMask{},validMask{},vertexPageId{};
    uint64_t dirtyStamp{};
    bool active{};
    radray::vector<TerrainWireframe> wireframes;
};
static_assert(std::is_nothrow_destructible_v<TerrainMesh>);

struct GeometryUpload {
    radray::IntrusivePtr<radray::GpuBuffer> target;
    uint64_t offset{};
    radray::RDGUploadSource source;
    // When present, the packed source fills these destination ranges in order.
    std::span<const radray::gpu::BufferRange> copyRanges{};
    void Import(radray::RDGBuilder& graph);
};

struct TerrainLayerDrawData {
    radray::PreparedMeshPassIndirectDraws prepared;
    radray::vector<radray::VisibleMeshDrawCommand> indirectReps;
    // Per-layer representative commands for the direct MDI fast path (owned independently per layer).
    radray::vector<radray::MeshDrawCommand> fastCommands;
    radray::MeshDrawCommandBindingStorage fastBindings;

    uint64_t indirectSize{};
    uint32_t recordCount{};
    uint64_t selectedGroups{};
    uint64_t indicesInstances{};

    void Clear() noexcept;
};

struct TerrainViewDrawData {
    radray::array<TerrainLayerDrawData,3> layers;
    // OIT 3 stages share layers[2].prepared and a single RDG IndirectArgs slice;
    // only the B representative VisibleMeshDrawCommands (typically B <= 4) are mapped per stage.
    radray::array<radray::vector<radray::VisibleMeshDrawCommand>,3> oitIndirectReps;
    bool oit{};
    bool wireframe{};
    bool hasAnyDraws{};
    uint32_t totalIndirectArguments{};

    void Clear() noexcept;
};

struct SlotPromotedBuffer {
    radray::IntrusivePtr<radray::GpuBuffer> buffer;
    uint64_t lastBytes{};
    uint32_t stableFrames{};
    // Failure preserves the cache and both output arguments.
    bool Prepare(radray::gpu::Device& device,bool unchanged,uint64_t bytes,radray::gpu::BufferUses usage,
        radray::IntrusivePtr<radray::GpuBuffer>& output,bool& upload,bool hasDraws=true);
};

struct TerrainViewCacheSlot {
    struct Layer {
        uint64_t geometryVersion{};
        TerrainEyeCrossingCache crossing;
        size_t visibleCount{};
        bool valid{};
    };
    TerrainViewDrawData draws;
    radray::array<Layer,3> layers;
    SlotPromotedBuffer instanceBuffer;
    radray::IntrusivePtr<radray::GpuBuffer> instancePool;
    bool pooledInstances{};
    radray::array<SlotPromotedBuffer,3> argumentBuffers;
    radray::vector<rn_visible_item> visible;
    radray::vector<rn_instance> instances;
    radray::array<radray::vector<uint32_t>,3> layerItems;
    uint64_t maxInstanceEnd{};
    uint64_t epoch{}, partitionVersion{}, visibleVersion{}, originsVersion{}, instancesVersion{};
    bool MatchesVisible(const rn_terrain_view_desc& view) const noexcept {
        return epoch==view.epoch && view.visible_version && visibleVersion==view.visible_version && visible.size()==view.visible_count;
    }
    bool MatchesOrigins(const rn_terrain_view_desc& view) const noexcept {
        return epoch==view.epoch && view.origins_version && originsVersion==view.origins_version && instances.size()==view.instance_count;
    }
    bool MatchesInstances(const rn_terrain_view_desc& view) const noexcept {
        return epoch==view.epoch && view.instances_version && instancesVersion==view.instances_version && instances.size()==view.instance_count;
    }
};

struct SlotUploads {
    radray::FrameSlot& frame;
    std::pmr::vector<GeometryUpload>& geometry;
    TerrainViewCacheSlot& cache;
    bool useIndirect{};
};

// ABI payloads are borrowed until synchronous RDG Prepare; no borrow is used by Wait or cleanup.
struct TerrainFrame {
    const TerrainViewDrawData* draws{};
    std::pmr::vector<GeometryUpload>* geometry{};
    std::span<const rn_instance> instances;
    radray::IntrusivePtr<radray::GpuBuffer> persistentInstances;
    radray::array<radray::IntrusivePtr<radray::GpuBuffer>,3> persistentIndirect;
    radray::array<bool,3> uploadIndirect{};
    bool uploadInstances{};
    PendingResourceOps* uploads{};
    std::span<const ShadowWrite> shadowWrites;
    rn_terrain_view_desc parameters{};
    bool hasTranslucent{};
    uint32_t width{},height{};
    radray::gpu::Texture* color{};
    radray::gpu::SwapChainSyncObject* waitToDraw{};
    radray::gpu::SwapChainSyncObject* readyToPresent{};
};

class TerrainResidency final {
public:
    static constexpr uint32_t kMaxMeshSlot=1048576u;
    TerrainResidency(radray::DeviceContext&,radray::unique_ptr<radray::VertexBufferPool> vertices,
        radray::unique_ptr<radray::VertexBufferPool> translucentVertices,
        radray::unique_ptr<radray::IndexBufferPool> quadPool,radray::unique_ptr<radray::IndexBufferPool> indices);
    void BeginSlotViews(std::span<const rn_terrain_view_desc> views,radray::FrameSlot& slot);
    TerrainViewCacheSlot& View(size_t index) { return *_views[index]; }
    bool PrepareFrame(rn_renderer* owner,const rn_terrain_view_desc&,const MeshPassResources&,SlotUploads,TerrainFrame&);
    void Reset() noexcept;
    uint64_t ResidentBytes() const noexcept;
    void MeasureGeometry(rn_frame_metrics&) const noexcept;
    const radray::VertexBufferPool& VertexPool() const noexcept { return *_vertices; }
    const radray::VertexBufferPool& TranslucentVertexPool() const noexcept { return *_translucentVertices; }
    const radray::IndexBufferPool& QuadIndexPool() const noexcept { return *_quadPool; }
    const radray::IndexBufferPool& TranslucentIndexPool() const noexcept { return *_indices; }
    const radray::IndexBufferSubAllocation& QuadIndices() const noexcept { return _quadIndices; }

    bool EnsureQuadIndices(uint32_t vertexCount,std::pmr::vector<GeometryUpload>& uploads,radray::FrameSlot* retireSlot,
        uint64_t* uploadBytesOut=nullptr,bool* grewOut=nullptr);
    bool ResyncQuadMeshes();
private:
    struct RetiredTerrainAllocations {
        radray::vector<radray::VertexBufferSubAllocation> vertices;
        radray::vector<radray::IndexBufferSubAllocation> indices;
    };
    RetiredTerrainAllocations& Retired(radray::FrameSlot&);
    void RetireMesh(TerrainMesh&,radray::FrameSlot&);
    RetiredTerrainAllocations* _retired{};
    struct TerrainState {
        uint64_t epoch{};
        radray::vector<TerrainMesh> meshesBySlot;
        uint32_t residentMeshCount{};
        bool wireframeActive{};
    };
    struct FastPageBucket {
        radray::vector<radray::gpu::DrawIndexedIndirectArguments> args;
        uint32_t repSlotId{};
        uint32_t wireMask{};
        bool hasNonZeroFirstInstance{};
    };
    struct FastOitBucket {
        uint16_t vertexPageId{};
        uint16_t indexPageId{};
        uint32_t repSlotId{};
        bool hasNonZeroFirstInstance{};
        radray::vector<radray::gpu::DrawIndexedIndirectArguments> args;
    };
    struct FastClassicTranslucentItem {
        uint64_t sortKey{};
        uint32_t rawSortKey{};
        uint32_t slotId{};
        uint16_t vertexPageId{};
        uint16_t indexPageId{};
        radray::gpu::DrawIndexedIndirectArguments arg{};
    };
    bool ValidateAndSyncHotSlot(uint32_t slotId);

    radray::DeviceContext& _device;
    radray::unique_ptr<radray::VertexBufferPool> _vertices;
    radray::unique_ptr<radray::VertexBufferPool> _translucentVertices;
    radray::unique_ptr<radray::IndexBufferPool> _quadPool,_indices;
    radray::IndexBufferSubAllocation _quadIndices;
    uint32_t _quadVertexCapacity{};
    TerrainState _state;
    radray::vector<TerrainHotSlot> _hotSlots;
    radray::vector<uint32_t> _changedSlotsScratch;
    uint64_t _syncStamp{};
    radray::vector<radray::unique_ptr<TerrainViewCacheSlot>> _views;
    uint64_t _partitionVersion{1};
    radray::array<uint64_t,3> _layerGeometryVersion{1,1,1};
    radray::array<radray::vector<FastPageBucket>,2> _fastPageBuckets;
    radray::vector<FastOitBucket> _fastOitBuckets;
    radray::unordered_map<uint32_t,size_t> _oitPageLookup;
    radray::array<radray::unordered_map<uint64_t,uint32_t>,2> _wirePages;
    radray::vector<FastClassicTranslucentItem> _fastClassicTranslucent;
    radray::vector<ProjectedTerrainBounds> _classicBounds;
    bool GroupDisjointClassicItems(std::span<const rn_instance>,const rn_terrain_view_desc&);
    radray::vector<uint32_t> _fastBatchRepSlots;
};
}
