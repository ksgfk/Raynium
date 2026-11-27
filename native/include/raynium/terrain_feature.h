#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>

#include <radray/gpu/types.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/frame_upload.h>
#include <raynium/terrain_residency.h>

namespace raynium {
bool SubmitPreparedTerrainDirectDraws(
    radray::gpu::GraphicsCommandEncoder& encoder,
    std::span<const radray::VisibleMeshDrawCommand> reps,
    const radray::PreparedMeshPassIndirectDraws& draws,
    const radray::MeshPassSubmitArgs& bindings) noexcept;

class TerrainView final {
public:
    explicit TerrainView(std::pmr::memory_resource& memory);

    radray::RDGBufferHandle Constants(radray::RDGBuilder& graph, uint32_t width, uint32_t height);
    void Import(radray::RDGBuilder& graph, radray::gpu::GpuBackend gpuBackend);

    TerrainViewDrawData* Draws{};
    TerrainFrame Frame;
    std::pmr::vector<GeometryUpload> Uploads;
    radray::RDGBufferHandle ConstantsBuffer;
    radray::RDGBufferHandle Instances;
    radray::array<radray::RDGBufferHandle, 3> IndirectByLayer;
    const MeshPassResources* Mesh{};
    const radray::VertexBufferPool* VertexPool{};
    const radray::VertexBufferPool* TranslucentVertexPool{};
    const radray::IndexBufferPool* QuadIndexPool{};
    const radray::IndexBufferPool* TranslucentIndexPool{};
    const radray::gpu::GpuDeviceCapabilities* Capabilities{};
    bool UseIndirect{};
    uint32_t IndirectLimit{};
    uint32_t AtlasWidth{};
    uint32_t AtlasHeight{};
    radray::gpu::GpuBackend Backend{};
};

class TerrainFeature final {
public:
    struct MaterialBinding {
        const rn_texture* Image{};
        rn_texture_sampling Sampling{};
        uint32_t Flags{};
        radray::RDGTextureHandle Handle;
    };

    bool Decode(std::span<const radray::byte> bytes, std::span<TerrainView> views, rn_renderer* owner, const radray::Rect& area);
    void Import(radray::RDGBuilder& graph, FrameTextureImports& imports);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx);
    bool Record(radray::gpu::GraphicsCommandEncoder& enc) const;

    TerrainView* View{};
    uint32_t Stage{};
    radray::Rect Scissor{};
    radray::array<const rn_texture*, 3> Sources{};
    radray::array<uint32_t, 3> Mips{};
    radray::array<radray::RDGTextureHandle, 3> Reads;
    radray::RDGBufferHandle FrameConstants;
    radray::array<MaterialBinding, 2> Materials;
    radray::RDGBufferSlice InstanceSlice{};
    radray::RDGBufferSlice IndirectSlice{};
    radray::gpu::ShaderParameterSet* Constants{};
    radray::gpu::ShaderParameterSet* Material{};
    radray::gpu::ShaderParameterSet* Oit{};
    bool Active{};
};
}  // namespace raynium
