#pragma once
#include <raynium/terrain_residency.h>
#include <raynium/frame_upload.h>

namespace raynium {
inline bool SubmitPreparedTerrainDirectDraws(radray::gpu::GraphicsCommandEncoder& encoder,
    std::span<const radray::VisibleMeshDrawCommand> reps,const radray::PreparedMeshPassIndirectDraws& draws,
    const radray::MeshPassSubmitArgs& bindings) noexcept {
    static const radray::gpu::GpuDeviceCapabilities direct{};
    return radray::SubmitMeshPassIndirectDraws(encoder,reps,draws,{},{},direct,bindings);
}

struct TerrainView {
    TerrainViewDrawData* draws{};
    TerrainFrame frame;
    radray::vector<GeometryUpload> uploads;
    radray::RDGBufferHandle constants,instances;
    radray::array<radray::RDGBufferHandle,3> indirectByLayer;
    const MeshPassResources* mesh{};
    const radray::VertexBufferPool* vertexPool{};
    const radray::VertexBufferPool* translucentVertexPool{};
    const radray::IndexBufferPool* quadIndexPool{},*translucentIndexPool{};
    const radray::gpu::GpuDeviceCapabilities* capabilities{};
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
    struct MaterialBinding {
        radray::IntrusivePtr<radray::GpuTexture> image;
        rn_texture_sampling sampling{};
        uint32_t flags{};
        radray::RDGTextureHandle handle;
    };
    radray::array<MaterialBinding,2> materials;
    radray::RDGBufferSlice instanceSlice{};
    radray::RDGBufferSlice indirectSlice{};
    radray::gpu::ShaderParameterSet* constants{};
    radray::gpu::ShaderParameterSet* material{};
    radray::gpu::ShaderParameterSet* oit{};
    bool active{};
    bool Decode(std::span<const radray::byte>,std::span<TerrainView>,const TextureRegistry&,const radray::Rect&);
    void Import(radray::RDGBuilder&,FrameTextureImports&);
    void Declare(radray::RDGRasterPassBuilder&) const;
    bool Prepare(radray::RDGPrepareContext&);
    bool Record(radray::gpu::GraphicsCommandEncoder&) const;
};
}
