#pragma once
#include <raynium/terrain_residency.h>
#include <raynium/frame_record.h>

namespace raynium {
struct TerrainView {
    TerrainFrameLists lists;
    TerrainFrame frame;
    radray::vector<GeometryUpload> uploads;
    radray::RDGBufferHandle constants,instances,indirect;
    const MeshPassResources* mesh{};
    bool useIndirect{};
    uint32_t indirectLimit{};
    void Import(radray::RDGBuilder&,radray::gpu::GpuBackend);
};
struct TerrainFeature {
    TerrainView* view{};
    uint32_t stage{};
    radray::Rect scissor{};
    radray::array<radray::IntrusivePtr<radray::GpuTexture>,3> sources;
    radray::array<uint32_t,3> mips{};
    radray::array<radray::RDGTextureHandle,3> reads;
    radray::vector<radray::RDGBufferHandle> vertexPages,indexPages;
    struct MaterialBinding {
        radray::IntrusivePtr<radray::GpuTexture> image;
        rn_texture_sampling sampling{};
        uint32_t flags{};
        radray::RDGTextureHandle handle;
    };
    radray::array<MaterialBinding,2> materials;
    TerrainRecord record;
    radray::gpu::ShaderParameterSet* constants{};
    radray::gpu::ShaderParameterSet* oit{};
    radray::gpu::GraphicsPipelineState* pipeline{};
    bool Decode(std::span<const radray::byte>,std::span<TerrainView>,const TextureRegistry&,const radray::Rect&);
    void Import(radray::RDGBuilder&,FrameTextureImports&);
    void Declare(radray::RDGRasterPassBuilder&) const;
    bool Prepare(radray::RDGPrepareContext&);
    bool Record(radray::gpu::GraphicsCommandEncoder&) const;
};
}
