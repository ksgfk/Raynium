#include <raynium/immediate_pipeline.h>
#include <algorithm>
#include <radray/logger.h>
#include <bit>
#include <cmath>

namespace raynium {
using namespace radray::gpu;

GraphicsPipelineState* ImmediatePipeline::Resolve(Device& device,TextureFormat depthFormat) {
    if(auto found=variants.find(depthFormat);found!=variants.end()) return found->second.get();
    std::optional<DepthStencilState> depth;
    if(depthFormat!=TextureFormat::UNKNOWN) {
        depth=state.depth.value_or(DepthStencilState{.DepthCompare=CompareFunction::Always});
        depth->Format=depthFormat;
    } else if(state.depth) return nullptr;
    using Stage=radray::shader::ShaderStage;
    auto pso=device.CreateGraphicsPipelineState({.PipelineLayout=program->layout.get(),
        .VS=ShaderEntry{program->vs.get(),program->program->Entry(Stage::Vertex)},
        .PS=ShaderEntry{program->ps.get(),program->program->Entry(Stage::Pixel)},
        .VertexInput={state.vertices,state.attributes},.Primitive=state.primitive,.DepthStencil=depth,
        .MultiSample=MultiSampleState::Default(),.ColorTargets=state.colors,.Cache=cache}).Release();
    if(!pso) return nullptr;
    auto* result=pso.get();
    variants.emplace(depthFormat,std::move(pso));
    return result;
}

radray::unique_ptr<ImmediatePipeline> ImmediatePipeline::Create(Device& device,RenderProgramLibrary& resources,const rn_pipeline_desc& desc,
    radray::Nullable<PipelineCache*> cache) {
    if(desc.struct_size!=sizeof(desc) || !desc.program || !desc.words || desc.word_count<9) return {};
    const auto* w=desc.words;
    const uint32_t topology=w[0],flags=w[1],compare=w[2],bindings=w[6],attributes=w[7],colors=w[8];
    if(topology>3 || flags>3 || compare>8 || w[3]>1 || bindings>device.GetDetail().MaxVertexInputBindings ||
       attributes>32 || colors>device.GetCapabilities().Limits.MaxColorAttachments ||
       desc.word_count!=9ull+3ull*bindings+4ull*attributes+9ull*colors) return {};
    const auto bias=std::bit_cast<float>(w[4]),slope=std::bit_cast<float>(w[5]);
    if(!std::isfinite(bias) || double(bias)<INT32_MIN || double(bias)>INT32_MAX || !std::isfinite(slope)) return {};
    auto item=radray::make_unique<ImmediatePipeline>();
    if(desc.shader_pack_size>ShaderPackMaxSize || (desc.shader_pack_size && !desc.shader_pack)) return {};
    item->program=resources.GetProgram(device,desc.program,{static_cast<const radray::byte*>(desc.shader_pack),size_t(desc.shader_pack_size)});
    if(!item->program) return {};
    item->cache=cache;
    auto& state=item->state;
    constexpr PrimitiveTopology modes[]{PrimitiveTopology::TriangleList,PrimitiveTopology::LineList,PrimitiveTopology::LineStrip,PrimitiveTopology::TriangleStrip};
    state.primitive.Topology=modes[topology];
    state.primitive.StripIndexFormat.reset();
    state.primitive.UnclippedDepth=false;
    state.primitive.FaceClockwise=FrontFace::CW;
    state.primitive.Cull=(flags&1)?CullMode::Back:CullMode::None;
    state.primitive.Poly=(flags&2)?PolygonMode::Line:PolygonMode::Fill;
    if(compare<8) state.depth=DepthStencilState{.DepthCompare=CompareFunction(compare),.DepthBias={int32_t(bias),slope,0},
        .DepthTestEnable=true,.DepthWriteEnable=w[3]!=0};
    w+=9;
    for(uint32_t i=0;i<bindings;++i,w+=3) {
        if(w[0]>=device.GetDetail().MaxVertexInputBindings || !w[1] || w[2]>1) return {};
        state.vertices.push_back({w[0],w[1],w[2]?VertexStepMode::Instance:VertexStepMode::Vertex});
    }
    auto inputs=item->program->program->Artifact().VertexInputs();
    for(uint32_t i=0;i<attributes;++i,w+=4) {
        if(!w[3] || w[3]>uint32_t(VertexFormat::FLOAT32X4)) return {};
        auto found=std::find_if(inputs.begin(),inputs.end(),[&](const auto& a){return a.SemanticIndex==w[1];});
        if(found!=inputs.end()) state.attributes.push_back({w[0],w[2],"TEXCOORD",w[1],VertexFormat(w[3]),found->Location});
    }
    if(state.attributes.size()!=inputs.size()) {
        RADRAY_ERR_LOG("Incomplete vertex interface for {}: supplied={} required={}",desc.program,state.attributes.size(),inputs.size());
        return {};
    }
    for(uint32_t i=0;i<colors;++i,w+=9) {
        auto format=ToTextureFormat(rn_pixel_format(w[0]));
        if(format==TextureFormat::UNKNOWN || IsDepthStencilFormat(format) || w[1]>15 || w[2]>1) return {};
        auto target=ColorTargetState::Default(format);
        target.WriteMask=ColorWrite(w[1]);
        if(w[2]) {
            if(w[3]>12 || w[4]>12 || w[5]>4 || w[6]>12 || w[7]>12 || w[8]>4) return {};
            target.Blend=BlendState{{BlendFactor(w[3]),BlendFactor(w[4]),BlendOperation(w[5])},
                                    {BlendFactor(w[6]),BlendFactor(w[7]),BlendOperation(w[8])}};
        }
        state.colors.push_back(target);
    }
    return item;
}

}
