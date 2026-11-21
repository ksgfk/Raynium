#pragma once
#include <radray/types.h>
#include <radray/inline_vector.h>
#include <raynium/buffer_registry.h>
#include <raynium/frame_upload.h>
#include <raynium/immediate_pipeline.h>
#include <raynium/static_geometry_registry.h>
#include <raynium/terrain_residency.h>
#include <span>

namespace raynium {
struct ShaderResourceBindings final {
    struct Binding {
        uint32_t kind, slot, id, format;
        uint64_t offset, size;
        rn_texture_sampling sampling;
    };
    static_assert(sizeof(Binding) == 48);
    struct Resource {
        Binding binding;
        radray::shared_ptr<BufferEntry> buffer;
        radray::IntrusivePtr<radray::GpuTexture> texture;
        radray::RDGBufferHandle bufferHandle;
        radray::RDGTextureHandle imageHandle;
    };

    radray::shared_ptr<ImmediatePipeline> pipeline;
    radray::InlineVector<Resource, 8> resources;
    radray::InlineVector<radray::byte, 128> push;
    radray::Rect scissor{};
    radray::gpu::GraphicsPipelineState* pso{};
    radray::array<radray::gpu::ShaderParameterSet*, 2> sets{};

    bool Decode(uint32_t pipelineId, const radray::Rect& scissorRect, uint32_t bindingCount, uint32_t pushBytes,
        std::span<const radray::byte> bindingBytes, std::span<const radray::byte> pushData,
        const ImmediatePipelineRegistry& pipelines, const BufferRegistry& buffers, const TextureRegistry& textures,
        std::span<const radray::gpu::TextureFormat> colors, const radray::Rect& area);
    void Import(radray::RDGBuilder& graph, FrameTextureImports& imports);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth);
    bool Bind(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept;
    size_t BindingCount() const noexcept { return resources.size() + 1; }
};

class InstancedGeometryFeature final {
public:
    bool Decode(std::span<const radray::byte> bytes, std::span<const radray::byte> instanceUploads,
        radray::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs, bool canIndirect, uint32_t indirectLimit,
        const ImmediatePipelineRegistry& pipelines, const BufferRegistry& buffers, const TextureRegistry& textures,
        const StaticGeometryRegistry& staticGeometry, const TerrainResidency& terrain,
        std::span<const radray::gpu::TextureFormat> colors, const radray::Rect& area);
    struct WireRun { int32_t meshId; uint32_t firstInstance, instanceCount, reserved; };
    bool DecodeRuns(std::span<const radray::byte> runs, uint32_t instanceCount, uint32_t stride,
        uint64_t offset, uint64_t uploadSize, std::span<const radray::gpu::VertexBufferLayout> layouts,
        const StaticGeometryRegistry& geometry, const radray::IndexBufferSubAllocation& quadIndices,
        radray::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs, bool canIndirect, uint32_t indirectLimit);
    void Import(radray::RDGBuilder& graph, FrameTextureImports& imports, radray::RDGBufferHandle instanceBuffer,
        radray::RDGBufferHandle indirectBuffer);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth);
    bool Record(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept;
    const radray::VertexBufferPool* StaticVertexPool() const noexcept { return _staticVertexPool; }
    const radray::IndexBufferPool* IndexPool() const noexcept { return _indexPool; }
    bool UsesIndirect() const noexcept { return _usesIndirect; }
    size_t DrawCount() const noexcept;
    size_t BindingCount() const noexcept { return _shader.BindingCount(); }
private:
    struct Batch {
        radray::gpu::Buffer* vertexBuffer{};
        uint64_t vertexBufferSize{};
        uint32_t firstVertexOrArgOffset{};
        uint32_t indexCountOrArgCount{};
        uint32_t firstInstance{};
        uint32_t instanceCount{};
        bool indirect{};
    };
    ShaderResourceBindings _shader;
    radray::InlineVector<Batch, 4> _batches;
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

}
