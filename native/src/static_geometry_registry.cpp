#include <raynium/static_geometry_registry.h>
#include <raynium/exception.h>
#include <radray/logger.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace raynium {
using namespace radray::gpu;

bool StaticGeometryRegistry::Create(Device& device, radray::unique_ptr<StaticGeometryRegistry>& out) {
    if (out) RADRAY_ABORT("StaticGeometryRegistry output occupied");
    const radray::array layouts{
        VertexBufferLayout{0, kModelVertexStride, VertexStepMode::Vertex}};
    const radray::array attributes{
        radray::VertexInputAttribute{0, 0, "TEXCOORD", 0, VertexFormat::FLOAT32X3},
        radray::VertexInputAttribute{0, 12, "TEXCOORD", 1, VertexFormat::FLOAT32X2},
        radray::VertexInputAttribute{0, 20, "TEXCOORD", 2, VertexFormat::SNORM8X4}};
    auto vertices = radray::VertexBufferPool::Create(device, {
        .VertexStride = kModelVertexStride,
        .PageVertexCapacity = (16u << 20) / kModelVertexStride,
        .AdditionalUsage = BufferUse::Resource,
        .Layouts = layouts,
        .Attributes = attributes,
        .DebugNamePrefix = "StaticVertices"}).Release();
    if (!vertices) {
        RADRAY_ERR_LOG("Cannot create StaticVertices vertex buffer pool");
        return false;
    }
    auto registry = radray::unique_ptr<StaticGeometryRegistry>(new StaticGeometryRegistry(std::move(vertices)));
    constexpr radray::array<float, 24> particle{
         1,-1,0, 1,1,0,    1,1,0, 1,0,0,
        -1, 1,0, 0,0,0,   -1,-1,0, 0,1,0};
    constexpr radray::array<float, 24> shadow{
        0,0,0, 0,0,0,    0,0,1, 0,0,0,
        1,0,1, 0,0,0,    1,0,0, 0,0,0};
    uint32_t pageId{};
    if (!registry->Store(kParticleUnitQuadId, std::as_bytes(std::span(particle)), 4, 6, pageId) ||
        !registry->Store(kShadowUnitQuadId, std::as_bytes(std::span(shadow)), 4, 6, pageId)) return false;
    out = std::move(registry);
    return true;
}

bool StaticGeometryRegistry::Upsert(int32_t meshId, std::span<const radray::byte> vertices, uint32_t vertexCount, uint32_t indexCount, uint32_t& outPageId) {
    if (meshId < kFirstUserId) {
        RADRAY_ERR_LOG("Reserved static geometry id={}", meshId);
        return false;
    }
    return Store(meshId, vertices, vertexCount, indexCount, outPageId);
}

bool StaticGeometryRegistry::Store(int32_t meshId, std::span<const radray::byte> vertices,
    uint32_t vertexCount, uint32_t indexCount, uint32_t& outPageId) {
    if (!vertexCount || vertexCount % 4 || vertexCount > kMaxModelVertices ||
        vertices.size() != uint64_t(vertexCount) * kModelVertexStride || indexCount != (vertexCount / 4) * 6) {
        RADRAY_ERR_LOG("Invalid rigid model mesh upsert: id={} bytes={} vertices={} indices={}",
            meshId, vertices.size(), vertexCount, indexCount);
        return false;
    }
    auto allocation = _vertices->Allocate(vertexCount);
    if (!allocation) {
        RADRAY_ERR_LOG("Cannot allocate rigid model vertices: id={} vertices={}", meshId, vertexCount);
        return false;
    }
    auto subAlloc = std::move(*allocation);
    const auto pageId = subAlloc.GetPageIndex();
    if (pageId > UINT16_MAX) {
        RADRAY_ERR_LOG("Static vertex page id exceeds wire range");
        return false;
    }
    if (size_t(meshId) >= _meshes.size()) _meshes.resize(size_t(meshId) + 1);
    radray::vector<radray::byte> owned(vertices.begin(), vertices.end());
    _pendingUploads.push_back({
        subAlloc.GetGpuBuffer(),
        subAlloc.GetByteOffset(),
        radray::RDGUploadSource::OwnedBytes(std::move(owned))});
    auto& mesh = _meshes[meshId];
    if (mesh.geometry) _retired.push_back(std::move(mesh.geometry));
    mesh = StaticMesh{std::move(subAlloc), vertexCount, indexCount};
    outPageId = pageId;
    _maxVertexCount = std::max(_maxVertexCount, vertexCount);
    return true;
}

bool StaticGeometryRegistry::Remove(int32_t meshId) {
    if (meshId < kFirstUserId) {
        RADRAY_ERR_LOG("Invalid rigid model mesh removal id={}", meshId);
        return false;
    }
    if (Find(meshId)) {
        auto& mesh = _meshes[meshId];
        _retired.push_back(std::move(mesh.geometry));
        mesh = {};
    }
    return true;
}

const StaticMesh* StaticGeometryRegistry::Find(int32_t meshId) const noexcept {
    return meshId >= 0 && size_t(meshId) < _meshes.size() && _meshes[meshId].geometry ? &_meshes[meshId] : nullptr;
}

bool StaticGeometryRegistry::PrepareQuadIndices(TerrainResidency& terrain, radray::FrameSlot& slot, radray::vector<GeometryUpload>& uploads) {
    if (!_maxVertexCount) return true;
    bool grew = false;
    if (!terrain.EnsureQuadIndices(_maxVertexCount, uploads, &slot, nullptr, &grew)) return false;
    if (grew && !terrain.ResyncQuadMeshes()) return false;
    return true;
}

void StaticGeometryRegistry::FlushPending(radray::FrameSlot& slot, radray::vector<GeometryUpload>& uploads) {
    for (auto& upload : _pendingUploads) {
        uploads.push_back(std::move(upload));
    }
    _pendingUploads.clear();
    if (!_retired.empty()) {
        slot.Emplace<radray::vector<radray::VertexBufferSubAllocation>>(std::move(_retired));
        _retired.clear();
    }
}

void StaticGeometryRegistry::MeasureGeometry(rn_frame_metrics& metrics) const noexcept {
    metrics.vertex_capacity += _vertices->GetCapacityBytes();
    metrics.geometry_allocated += _vertices->GetAllocatedBytes();
}

void StaticGeometryRegistry::Reset() noexcept {
    _meshes.clear();
    _pendingUploads.clear();
    _retired.clear();
    _maxVertexCount = 0;
}
}
