#pragma once
#include <raynium/buffer_pool.h>
#include <raynium/frame_plan.h>
#include <raynium/frame_upload.h>
#include <raynium/immediate_pipeline.h>
#include <radray/inline_vector.h>

namespace raynium {
class ImmediateDraw final {
public:
    bool Decode(std::pmr::memory_resource& memory,std::span<const radray::byte>,rn_renderer* owner,
        std::span<const radray::gpu::TextureFormat> colors,const radray::Rect& area);
    void Import(radray::RDGBuilder&,FrameTextureImports&);
    void Declare(radray::RDGRasterPassBuilder&) const;
    bool Prepare(radray::RDGPrepareContext&,radray::gpu::TextureFormat depth);
    size_t BindingCount() const { return _resources.size()+1; }
    bool Record(radray::gpu::GraphicsCommandEncoder&,ImmediatePassStateCache&) const noexcept;
private:
    struct Binding {
        uint64_t resource,offset,size;
        uint8_t kind,format;
        uint16_t slot;
        uint32_t reserved;
        rn_texture_sampling sampling;
    };
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
    static_assert(sizeof(Resource)==16 && std::is_trivially_destructible_v<Resource>);
    static Binding ReadBinding(const radray::byte* base,size_t index) noexcept;
    ImmediatePipeline* _pipeline;
    const radray::byte* _bindingBytes{};
    std::span<Resource> _resources;
    std::span<const radray::byte> _push;
    radray::Rect _scissor{};
    uint32_t _kind{},_count{},_instances{},_first{},_firstInstance{};
    int32_t _base{};
    radray::gpu::GraphicsPipelineState* _pso{};
    radray::array<radray::gpu::ShaderParameterSet*,2> _sets{};
    std::span<radray::gpu::VertexBufferBinding> _vertices;
    radray::gpu::IndexBufferView _indices{};
    radray::RDGBufferSlice _indirect{};
    radray::RDGBufferHandle _fan,_drawBases;
    std::span<radray::gpu::ShaderParameterSet*> _drawBaseSets;
};
}
