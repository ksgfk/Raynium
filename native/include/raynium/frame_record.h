#pragma once
#include <raynium/gui_pass.h>
#include <raynium/terrain_draw.h>

namespace raynium {
struct TerrainRecord {
    const TerrainFrameLists* lists{};
    uint32_t layer{},nativeLimit{};
    bool useIndirect{};
    radray::RDGBufferSlice instances{},indirect{};
};
// Only encoder commands are emitted; all buffers, parameters and draw lists are prepared and immutable.
bool RecordTerrainDraws(const TerrainRecord&,radray::render::GraphicsCommandEncoder&) noexcept;
bool RecordGuiDraws(const GuiFrameSlot&,std::span<radray::render::GraphicsPipelineState* const>,
    radray::RDGBufferSlice vertices,radray::RDGBufferSlice indices,radray::render::GraphicsCommandEncoder&) noexcept;
}
