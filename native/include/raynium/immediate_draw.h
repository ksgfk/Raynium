#pragma once
#include <raynium/buffer_registry.h>
#include <raynium/immediate_pipeline.h>
#include <radray/inline_vector.h>

namespace raynium {
class ImmediateDraw final {
public:
    bool Decode(std::span<const radray::byte>,const ImmediatePipelineRegistry&,const BufferRegistry&,const TextureRegistry&,
        std::span<const radray::gpu::TextureFormat> colors,const radray::Rect& area);
    void Import(radray::RDGBuilder&,FrameTextureImports&);
    void Declare(radray::RDGRasterPassBuilder&) const;
    bool Prepare(radray::RDGPrepareContext&,radray::gpu::TextureFormat depth);
    size_t BindingCount() const { return _resources.size()+1; }
    bool Record(radray::gpu::GraphicsCommandEncoder&) const noexcept;
private:
    struct Binding {
        uint32_t kind,slot,id,format;
        uint64_t offset,size;
        rn_texture_sampling sampling;
    };
    struct Resource {
        Binding binding;
        radray::shared_ptr<BufferEntry> buffer;
        radray::IntrusivePtr<radray::GpuTexture> texture;
        radray::RDGBufferHandle bufferHandle;
        radray::RDGTextureHandle imageHandle;
    };
    radray::shared_ptr<ImmediatePipeline> _pipeline;
    radray::InlineVector<Resource,8> _resources;
    radray::InlineVector<radray::byte,128> _push;
    radray::Rect _scissor{};
    uint32_t _kind{},_count{},_instances{},_first{},_firstInstance{};
    int32_t _base{};
    radray::gpu::GraphicsPipelineState* _pso{};
    radray::array<radray::gpu::ShaderParameterSet*,2> _sets{};
    radray::InlineVector<radray::gpu::VertexBufferBinding,2> _vertices;
    radray::gpu::IndexBufferView _indices{};
    radray::RDGBufferSlice _indirect{};
    radray::RDGBufferHandle _fan,_drawBases;
    radray::InlineVector<radray::gpu::ShaderParameterSet*,1> _drawBaseSets;
};
}
