#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <type_traits>

#include <radray/inline_vector.h>
#include <radray/types.h>
#include <raynium/buffer_pool.h>
#include <raynium/frame_plan.h>
#include <raynium/frame_upload.h>
#include <raynium/immediate_pipeline.h>
#include <raynium/static_geometry_registry.h>
#include <raynium/terrain_residency.h>

namespace raynium {
class ShaderResourceBindings final {
public:
    struct Binding {
        uint64_t Resource{};
        uint64_t Offset{};
        uint64_t Size{};
        uint8_t Kind{};
        uint8_t Format{};
        uint16_t Slot{};
        uint32_t Reserved{};
        rn_texture_sampling Sampling{};
    };
    static_assert(sizeof(Binding) == 48);

    struct Resource {
        union {
            rn_buffer* Buffer{};
            const rn_texture* Texture;
        };
        union {
            radray::RDGBufferHandle BufferHandle{};
            radray::RDGTextureHandle ImageHandle;
        };
        uint8_t Kind{};
    };
    static_assert(sizeof(Resource) == 16 && std::is_trivially_destructible_v<Resource>);

    static Binding ReadBinding(const radray::byte* base, size_t index) noexcept;

    bool Decode(
        std::pmr::memory_resource& memory,
        rn_pipeline* pipelineId,
        const radray::Rect& scissorRect,
        uint32_t bindingCount,
        uint32_t pushBytes,
        std::span<const radray::byte> bindingSpan,
        std::span<const radray::byte> pushData,
        rn_renderer* owner,
        std::span<const radray::gpu::TextureFormat> colors,
        const radray::Rect& area);
    void Import(radray::RDGBuilder& graph, FrameTextureImports& imports);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth);
    bool Bind(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept;
    size_t BindingCount() const noexcept;

    ImmediatePipeline* Pipeline{};
    const radray::byte* BindingBytes{};
    std::span<Resource> Resources;
    std::span<const radray::byte> Push;
    radray::Rect Scissor{};
    radray::gpu::GraphicsPipelineState* Pso{};
    radray::array<radray::gpu::ShaderParameterSet*, 2> Sets{};
};

class InstancedGeometryFeature final {
public:
    struct WireRun {
        int32_t MeshId{};
        uint32_t FirstInstance{};
        uint32_t InstanceCount{};
        uint32_t Reserved{};
    };

    bool Decode(
        std::pmr::memory_resource& memory,
        std::span<const radray::byte> bytes,
        std::span<const radray::byte> instanceUploads,
        std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,
        bool canIndirect,
        uint32_t indirectLimit,
        rn_renderer* owner,
        const StaticGeometryRegistry& staticGeometry,
        const TerrainResidency& terrain,
        std::span<const radray::gpu::TextureFormat> colors,
        const radray::Rect& area);
    bool DecodeRuns(
        std::pmr::memory_resource& memory,
        std::span<const radray::byte> runs,
        uint32_t instanceCount,
        uint32_t stride,
        uint64_t offset,
        uint64_t uploadSize,
        std::span<const radray::gpu::VertexBufferLayout> layouts,
        const StaticGeometryRegistry& geometry,
        const radray::IndexBufferSubAllocation& quadIndices,
        std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,
        bool canIndirect,
        uint32_t indirectLimit);
    void Import(
        radray::RDGBuilder& graph,
        FrameTextureImports& imports,
        radray::RDGBufferHandle instanceBuffer,
        radray::RDGBufferHandle indirectBuffer);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth);
    bool Record(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept;
    const radray::VertexBufferPool* StaticVertexPool() const noexcept;
    const radray::IndexBufferPool* IndexPool() const noexcept;
    bool UsesIndirect() const noexcept;
    size_t DrawCount() const noexcept;
    size_t BindingCount() const noexcept;

private:
    struct Batch {
        radray::gpu::Buffer* VertexBuffer{};
        uint64_t VertexBufferSize{};
        uint32_t FirstVertexOrArgOffset{};
        uint32_t IndexCountOrArgCount{};
        uint32_t FirstInstance{};
        uint32_t InstanceCount{};
        bool Indirect{};
    };

    ShaderResourceBindings _shader;
    std::span<Batch> _batches;
    const radray::VertexBufferPool* _staticVertexPool{};
    const radray::IndexBufferPool* _indexPool{};
    radray::gpu::IndexBufferView _indexView{};
    uint32_t _quadFirstIndex{};
    uint32_t _indirectLimit{1};
    bool _usesIndirect{};
    uint64_t _uploadOffset{};
    uint64_t _instanceBytes{};
    radray::RDGBufferHandle _instanceBuffer;
    radray::RDGBufferHandle _indirectBuffer;
    radray::RDGBufferSlice _instanceSlice{};
    radray::RDGBufferSlice _indirectSlice{};
};
}  // namespace raynium
