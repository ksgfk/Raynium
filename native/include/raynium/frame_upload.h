#pragma once
#include <radray/types.h>
#include <radray/inline_vector.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/transient_parameter_pool.h>
#include <algorithm>

namespace raynium {
struct ImmediatePassStateCache {
    radray::gpu::GraphicsPipelineState* pso{};
    radray::gpu::PipelineLayout* layout{};
    radray::Rect scissor{-1,-1,0,0};
    uint32_t stencil{UINT32_MAX};
    radray::array<radray::gpu::ShaderParameterSet*,2> sets{};
    radray::gpu::IndexBufferView indexView{};
    bool hasIndexView{};

    void Invalidate() noexcept { *this=ImmediatePassStateCache{}; }

    void BindPipeline(radray::gpu::GraphicsCommandEncoder& enc,radray::gpu::GraphicsPipelineState* nextPso) noexcept {
        if(pso==nextPso) return;
        auto* nextLayout=nextPso?nextPso->GetLayout().Get():nullptr;
        const bool sameLayout=pso!=nullptr && nextLayout!=nullptr && layout==nextLayout;
        enc.BindGraphicsPipelineState(nextPso);
        pso=nextPso;
        layout=nextLayout;
        if(!sameLayout) sets={};
    }

    void SetScissor(radray::gpu::GraphicsCommandEncoder& enc,const radray::Rect& next) noexcept {
        if(scissor.X==next.X && scissor.Y==next.Y && scissor.Width==next.Width && scissor.Height==next.Height) return;
        enc.SetScissor(next);
        scissor=next;
    }

    void SetStencil(radray::gpu::GraphicsCommandEncoder& enc,uint32_t next) noexcept {
        if(stencil==next) return;
        enc.SetStencilReference(uint8_t(next));
        stencil=next;
    }

    void BindParameterSet(radray::gpu::GraphicsCommandEncoder& enc,uint32_t group,radray::gpu::ShaderParameterSet* set) noexcept {
        if(group<sets.size()) {
            if(sets[group]==set) return;
            sets[group]=set;
        }
        enc.BindShaderParameterSet(group,set);
    }

    void BindIndexBuffer(radray::gpu::GraphicsCommandEncoder& enc,const radray::gpu::IndexBufferView& next) noexcept {
        if(hasIndexView && indexView.Target==next.Target && indexView.Offset==next.Offset && indexView.Stride==next.Stride) return;
        enc.BindIndexBuffer(next);
        indexView=next;
        hasIndexView=true;
    }
};

template<size_t N>
inline void EnsureOrderedBindings(radray::InlineVector<radray::TransientParameterBinding,N>& group) noexcept {
    if(group.size()<=1) return;
    bool sorted=true;
    for(size_t i=1;i<group.size();++i) {
        const auto& a=group[i-1];
        const auto& b=group[i];
        if(a.BindingId>b.BindingId || (a.BindingId==b.BindingId && a.ArrayElement>b.ArrayElement)) {
            sorted=false;
            break;
        }
    }
    if(!sorted) {
        std::sort(group.begin(),group.end(),[](const radray::TransientParameterBinding& a,const radray::TransientParameterBinding& b) noexcept {
            return a.BindingId!=b.BindingId?a.BindingId<b.BindingId:a.ArrayElement<b.ArrayElement;
        });
    }
}

radray::TransientParameterBinding FrameConstantBinding(uint32_t binding,radray::RDGBufferSlice slice) noexcept;
radray::gpu::VertexBufferView FrameVertexView(radray::RDGBufferSlice) noexcept;
radray::gpu::IndexBufferView FrameIndexView(radray::RDGBufferSlice) noexcept;
uint64_t FrameIndirectOffset(radray::RDGBufferSlice,uint32_t record) noexcept;
}
