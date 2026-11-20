#pragma once
#include <raynium/frame_upload.h>
#include <raynium/terrain_draw.h>


namespace raynium {
struct TerrainRecord {
    const TerrainFrameLists* lists{};
    uint32_t layer{},nativeLimit{};
    bool useIndirect{};
    radray::RDGBufferSlice instances{},indirect{};
    radray::gpu::ShaderParameterSet* material{};
};
// Only encoder commands are emitted; all buffers, parameters and draw lists are prepared and immutable.
bool RecordTerrainDraws(const TerrainRecord&,radray::gpu::GraphicsCommandEncoder&) noexcept;
}
