#pragma once
#include <raynium/gui_pass.h>
#include <raynium/terrain_draw.h>
#include <raynium/scene_pass.h>

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
    radray::RDGBufferSlice vertices,radray::RDGBufferSlice indices,radray::render::GraphicsCommandEncoder&,uint32_t first=0,uint32_t count=UINT32_MAX) noexcept;
bool RecordSceneDraws(std::span<const SceneFrameSlot::PhaseDraw>,radray::render::GraphicsCommandEncoder&) noexcept;
bool RecordSceneScreen(const SceneDraw&,uint32_t width,uint32_t height,radray::render::GraphicsCommandEncoder&) noexcept;
}
