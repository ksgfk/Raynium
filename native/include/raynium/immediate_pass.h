#pragma once
#include <raynium/immediate_draw.h>
#include <variant>
#include <raynium/terrain_feature.h>
#include <raynium/texture_registry.h>
#include <raynium/timestamp_queries.h>

namespace raynium {
class ImmediatePass final {
public:
    static radray::shared_ptr<ImmediatePass> Decode(std::span<const radray::byte> record,const TextureRegistry&,uint32_t maxColors);
    bool AddTerrain(std::span<const radray::byte>,std::span<TerrainView>,const TextureRegistry&);
    bool AddDraw(std::span<const radray::byte>,const ImmediatePipelineRegistry&,const BufferRegistry&,const TextureRegistry&);
    void Measure(rn_frame_metrics&) const;
    void WriteTimestamp(uint32_t physicalIndex);
    void Replay(radray::RDGBuilder&,FrameTextureImports&,const FrameTimestamps&);
private:
    struct Attachment {
        radray::IntrusivePtr<radray::GpuTexture> texture;
        uint32_t mip{},layer{};
        bool clear{};
        radray::gpu::ColorClearValue value{};
    };
    radray::Rect _area{};
    uint32_t _width{},_height{};
    radray::vector<Attachment> _colors;
    Attachment _depth;
    radray::vector<std::variant<uint32_t,ImmediateDraw,TerrainFeature>> _commands;
    bool _containsTimestamp{};
};
}
