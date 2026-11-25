#pragma once
#include <radray/types.h>
#include <radray/inline_vector.h>
#include <raynium/buffer_pool.h>
#include <raynium/frame_plan.h>
#include <raynium/frame_upload.h>
#include <raynium/immediate_pipeline.h>
#include <raynium/static_geometry_registry.h>
#include <raynium/terrain_residency.h>
#include <span>

namespace raynium {
struct ShaderResourceBindings final {
    struct Binding {
        uint64_t resource,offset,size;
        uint8_t kind,format;
        uint16_t slot;
        uint32_t reserved;
        rn_texture_sampling sampling;
    };
    static_assert(sizeof(Binding) == 48);
    struct Resource {
        union {
            rn_buffer* buffer{};
            const rn_texture* texture;
        };
        union {
            radray::RDGBufferHandle bufferHandle{};
            radray::RDGTextureHandle imageHandle;
        };
        uint8_t kind{};
    };
    static_assert(sizeof(Resource) == 16 && std::is_trivially_destructible_v<Resource>);
    static Binding ReadBinding(const radray::byte* base, size_t index) noexcept;

    ImmediatePipeline* pipeline;
    const radray::byte* bindingBytes{};
    std::span<Resource> resources;
    std::span<const radray::byte> push;
    radray::Rect scissor{};
    radray::gpu::GraphicsPipelineState* pso{};
    radray::array<radray::gpu::ShaderParameterSet*, 2> sets{};

    bool Decode(std::pmr::memory_resource& memory,rn_pipeline* pipelineId, const radray::Rect& scissorRect, uint32_t bindingCount, uint32_t pushBytes,
        std::span<const radray::byte> bindingBytes, std::span<const radray::byte> pushData,
        rn_renderer* owner,
        std::span<const radray::gpu::TextureFormat> colors, const radray::Rect& area);
    void Import(radray::RDGBuilder& graph, FrameTextureImports& imports);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth);
    bool Bind(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept;
    size_t BindingCount() const noexcept { return resources.size() + 1; }
};

class InstancedGeometryFeature final {
public:
    bool Decode(std::pmr::memory_resource& memory,std::span<const radray::byte> bytes, std::span<const radray::byte> instanceUploads,
        std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs, bool canIndirect, uint32_t indirectLimit,
        rn_renderer* owner,
        const StaticGeometryRegistry& staticGeometry, const TerrainResidency& terrain,
        std::span<const radray::gpu::TextureFormat> colors, const radray::Rect& area);
    struct WireRun { int32_t meshId; uint32_t firstInstance, instanceCount, reserved; };
    bool DecodeRuns(std::pmr::memory_resource& memory,std::span<const radray::byte> runs, uint32_t instanceCount, uint32_t stride,
        uint64_t offset, uint64_t uploadSize, std::span<const radray::gpu::VertexBufferLayout> layouts,
        const StaticGeometryRegistry& geometry, const radray::IndexBufferSubAllocation& quadIndices,
        std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs, bool canIndirect, uint32_t indirectLimit);
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

}
