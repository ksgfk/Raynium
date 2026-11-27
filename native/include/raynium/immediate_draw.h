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

namespace raynium {
class ImmediateDraw final {
public:
    bool Decode(
        std::pmr::memory_resource& memory,
        std::span<const radray::byte> bytes,
        rn_renderer* owner,
        std::span<const radray::gpu::TextureFormat> colors,
        const radray::Rect& area);
    void Import(radray::RDGBuilder& graph, FrameTextureImports& imports);
    void Declare(radray::RDGRasterPassBuilder& pass) const;
    bool Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth);
    size_t BindingCount() const noexcept;
    bool Record(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept;

private:
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

    ImmediatePipeline* _pipeline{};
    const radray::byte* _bindingBytes{};
    std::span<Resource> _resources;
    std::span<const radray::byte> _push;
    radray::Rect _scissor{};
    uint32_t _kind{};
    uint32_t _count{};
    uint32_t _instances{};
    uint32_t _first{};
    uint32_t _firstInstance{};
    int32_t _base{};
    radray::gpu::GraphicsPipelineState* _pso{};
    radray::array<radray::gpu::ShaderParameterSet*, 2> _sets{};
    std::span<radray::gpu::VertexBufferBinding> _vertices;
    radray::gpu::IndexBufferView _indices{};
    radray::RDGBufferSlice _indirect{};
    radray::RDGBufferHandle _fan;
    radray::RDGBufferHandle _drawBases;
    std::span<radray::gpu::ShaderParameterSet*> _drawBaseSets;
};
}  // namespace raynium
