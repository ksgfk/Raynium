#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>

#include <raynium/immediate_draw.h>
#include <raynium/resource_uploads.h>
#include <raynium/terrain_feature.h>
#include <raynium/timestamp_queries.h>
#include <raynium/world_features.h>

namespace raynium {
using RasterOp = FrameOp<uint32_t, ImmediateDraw, TerrainFeature, InstancedGeometryFeature>;

class RasterPassPlan final {
public:
    static RasterPassPlan* Decode(
        std::pmr::memory_resource& memory,
        std::span<const radray::byte> record,
        rn_renderer* owner,
        uint32_t maxColors);
    bool TryFoldClear(const rn_texture* texture, uint32_t mip, uint32_t layer, const radray::gpu::ColorClearValue& value) noexcept;
    bool AddTerrain(
        std::pmr::memory_resource& memory,
        std::pmr::vector<RasterOp>& ops,
        std::span<const radray::byte> bytes,
        std::span<TerrainView> views,
        rn_renderer* owner);
    bool AddDraw(
        std::pmr::memory_resource& memory,
        std::pmr::vector<RasterOp>& ops,
        std::span<const radray::byte> bytes,
        rn_renderer* owner);
    bool AddInstancedGeometry(
        std::pmr::memory_resource& memory,
        std::pmr::vector<RasterOp>& ops,
        std::span<const radray::byte> bytes,
        std::span<const radray::byte> instanceUploads,
        std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,
        bool canIndirect,
        uint32_t indirectLimit,
        rn_renderer* owner,
        const StaticGeometryRegistry& staticGeometry,
        const TerrainResidency& terrain);
    void Measure(rn_frame_metrics& metrics) const;
    void WriteTimestamp(std::pmr::vector<RasterOp>& ops, uint32_t physicalIndex);
    void Finish(std::pmr::memory_resource& memory, const std::pmr::vector<RasterOp>& ops);
    void Replay(
        radray::RDGBuilder& graph,
        FrameTextureImports& imports,
        const FrameTimestamps& timestamps,
        radray::RDGBufferHandle instanceBuffer,
        radray::RDGBufferHandle geometryIndirectBuffer);

private:
    struct Attachment {
        const rn_texture* Texture{};
        radray::RDGTextureHandle Handle;
        uint32_t Mip{};
        uint32_t Layer{};
        bool Clear{};
        radray::gpu::ColorClearValue Value{};
    };

    radray::Rect _area{};
    uint32_t _width{};
    uint32_t _height{};
    std::span<Attachment> _colors;
    std::span<radray::gpu::TextureFormat> _colorFormats;
    Attachment _depth;
    std::span<RasterOp> _commands;
    const radray::VertexBufferPool* _terrainVertices{};
    const radray::VertexBufferPool* _translucentVertices{};
    const radray::VertexBufferPool* _staticVertices{};
    const radray::IndexBufferPool* _quadIndices{};
    const radray::IndexBufferPool* _translucentIndices{};
    bool _needInstanceBuffer{};
    bool _needGeometryIndirect{};
    bool _containsTimestamp{};
};
}  // namespace raynium
