#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <raynium/terrain_residency.h>
#include <span>

namespace raynium {
struct StaticMesh {
    radray::VertexBufferSubAllocation geometry;
    uint32_t vertexCount{};
    uint32_t indexCount{};
};
static_assert(std::is_nothrow_destructible_v<StaticMesh>);

class StaticGeometryRegistry final {
public:
    static constexpr uint32_t kModelVertexStride = 24;
    static constexpr int32_t kParticleUnitQuadId = 1;
    static constexpr int32_t kShadowUnitQuadId = 2;
    static constexpr int32_t kFirstUserId = 16;
    static constexpr uint32_t kMaxModelVertices = 1048576;

    static bool Create(radray::gpu::Device& device, radray::unique_ptr<StaticGeometryRegistry>& out);
    ~StaticGeometryRegistry() noexcept = default;
    StaticGeometryRegistry(const StaticGeometryRegistry&) = delete;
    StaticGeometryRegistry& operator=(const StaticGeometryRegistry&) = delete;

    bool Upsert(int32_t meshId, std::span<const radray::byte> vertices, uint32_t vertexCount, uint32_t indexCount, uint32_t& outPageId);
    bool Remove(int32_t meshId);
    const StaticMesh* Find(int32_t meshId) const noexcept;
    bool PrepareQuadIndices(TerrainResidency& terrain, radray::FrameSlot& slot, radray::vector<GeometryUpload>& uploads);
    void FlushPending(radray::FrameSlot& slot, radray::vector<GeometryUpload>& uploads);
    void MeasureGeometry(rn_frame_metrics& metrics) const noexcept;
    void Reset() noexcept;

    const radray::VertexBufferPool& VertexPool() const noexcept { return *_vertices; }
    void BeginSlot() { _vertices->TrimEmptyPages(); }

private:
    explicit StaticGeometryRegistry(radray::unique_ptr<radray::VertexBufferPool> vertices) noexcept
        : _vertices(std::move(vertices)) {}
    bool Store(int32_t meshId, std::span<const radray::byte> vertices, uint32_t vertexCount,
        uint32_t indexCount, uint32_t& outPageId);

    radray::unique_ptr<radray::VertexBufferPool> _vertices;
    radray::vector<StaticMesh> _meshes;
    radray::vector<GeometryUpload> _pendingUploads;
    radray::vector<radray::VertexBufferSubAllocation> _retired;
    uint32_t _maxVertexCount{};
};
}
