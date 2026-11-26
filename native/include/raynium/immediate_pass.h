#pragma once
#include <raynium/immediate_draw.h>

#include <raynium/terrain_feature.h>
#include <raynium/world_features.h>
#include <raynium/resource_uploads.h>
#include <raynium/timestamp_queries.h>

namespace raynium {
using RasterOp=FrameOp<uint32_t,ImmediateDraw,TerrainFeature,InstancedGeometryFeature>;
class RasterPassPlan final {
public:
    static RasterPassPlan* Decode(std::pmr::memory_resource& memory,std::span<const radray::byte> record,rn_renderer* owner,uint32_t maxColors);
    bool TryFoldClear(const rn_texture*,uint32_t mip,uint32_t layer,const radray::gpu::ColorClearValue&) noexcept;
    bool AddTerrain(std::pmr::memory_resource& memory,std::pmr::vector<RasterOp>& ops,std::span<const radray::byte>,std::span<TerrainView>,rn_renderer* owner);
    bool AddDraw(std::pmr::memory_resource& memory,std::pmr::vector<RasterOp>& ops,std::span<const radray::byte>,rn_renderer* owner);
    bool AddInstancedGeometry(std::pmr::memory_resource& memory,std::pmr::vector<RasterOp>& ops,std::span<const radray::byte>,std::span<const radray::byte> instanceUploads,
        std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,bool canIndirect,uint32_t indirectLimit,
        rn_renderer* owner,const StaticGeometryRegistry&,const TerrainResidency&);
    void Measure(rn_frame_metrics&) const;
    void WriteTimestamp(std::pmr::vector<RasterOp>&,uint32_t);
    void Finish(std::pmr::memory_resource& memory,const std::pmr::vector<RasterOp>& ops) { _commands=FreezeFrameArray(memory,ops); }
    void Replay(radray::RDGBuilder&,FrameTextureImports&,const FrameTimestamps&,radray::RDGBufferHandle instanceBuffer,
        radray::RDGBufferHandle geometryIndirectBuffer);
private:
    struct Attachment {
        const rn_texture* texture;
        radray::RDGTextureHandle handle;
        uint32_t mip{},layer{};
        bool clear{};
        radray::gpu::ColorClearValue value{};
    };
    radray::Rect _area{};
    uint32_t _width{},_height{};
    std::span<Attachment> _colors;
    std::span<radray::gpu::TextureFormat> _colorFormats;
    Attachment _depth;
    std::span<RasterOp> _commands;
    const radray::VertexBufferPool *terrainVertices{},*translucentVertices{},*staticVertices{};
    const radray::IndexBufferPool *quadIndices{},*translucentIndices{};
    bool needInstanceBuffer{},needGeometryIndirect{};
    bool _containsTimestamp{};
};
}
