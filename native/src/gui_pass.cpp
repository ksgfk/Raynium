#include <raynium/gui_pass.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/terrain_draw.h>
#include <raynium/frame_record.h>
#include <radray/logger.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::render;
bool GuiPassResources::Create(Device& device,std::span<const radray::byte> shaders,radray::unique_ptr<GuiPassResources>& output) {
    if(output) RADRAY_ABORT("GUI resources output occupied");
    auto objects=radray::make_unique<Objects>(); auto& p=*objects;
    radray::vector<ShaderPackEntry> entries;
    if(!ReadShaderPack(shaders,entries)) return false;
    const bool vk=device.GetBackend()==RenderBackend::Vulkan;
    const BindingDescriptor pass{0,BindingKind::CBuffer,1,ShaderStage::Vertex|ShaderStage::Pixel,
        D3D12BindingDescriptor{BindingRegisterClass::CBV,0,0},VulkanBindingDescriptor{0,0}};
    const BindingDescriptor textures[]{
        {0,BindingKind::Texture,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::SRV,0,1},VulkanBindingDescriptor{1,0}},
        {1,BindingKind::Sampler,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::Sampler,0,1},VulkanBindingDescriptor{1,1}}};
    p.passLayout=device.CreateBindingGroupLayout({{&pass,1}}).Release();
    p.textureLayout=device.CreateBindingGroupLayout({textures}).Release();
    if(!p.passLayout || !p.textureLayout) { RADRAY_ERR_LOG("Cannot create GUI bindings"); return false; }
    for(size_t i=0;i<GuiPrograms.size();++i) {
        auto& program=p.programs[i];
        if(!ShaderProgram::Create(entries,GuiPrograms[i],vk?radray::shader::ShaderTarget::SPIRV:radray::shader::ShaderTarget::DXIL,program.program) ||
           !ValidateGuiShader(program.program->Artifact(),GuiPrograms[i])) return false;
        const auto& artifact=program.program->Artifact(); const auto blob=vk?ShaderBlobCategory::SPIRV:ShaderBlobCategory::DXIL;
        program.vs=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex),blob,ShaderStage::Vertex}).Release();
        program.ps=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel),blob,ShaderStage::Pixel}).Release();
        const PipelineLayoutGroupDescriptor groups[]{{0,p.passLayout.get()},{1,p.textureLayout.get()}};
        program.layout=device.CreatePipelineLayout({{groups,i==0?1u:2u},{}}).Release();
        if(!program.vs || !program.ps || !program.layout) { RADRAY_ERR_LOG("Cannot create GUI shader stages/layout"); return false; }
    }
    auto resources=radray::unique_ptr<GuiPassResources>(new GuiPassResources(std::move(objects)));
    if(!resources->PrepareOutput(device,TextureFormat::BGRA8_UNORM)) return false;
    output=std::move(resources); return true;
}
bool GuiPassResources::PrepareOutput(Device& device,TextureFormat format) {
    if(format!=TextureFormat::BGRA8_UNORM) { RADRAY_ERR_LOG("Unsupported GUI output format"); return false; }
    auto& pipelines=_objects->pipelines;
    const VertexBufferLayout vertex{0,24,VertexStepMode::Vertex};
    auto primitive=PrimitiveState::Default(); primitive.Cull=CullMode::None;
    const BlendState translucent{{BlendFactor::SrcAlpha,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add},
        {BlendFactor::One,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add}};
    for(uint32_t i=0;i<pipelines.size();++i) {
        if(pipelines[i]) continue;
        const bool color=i<=RN_GUI_HIGHLIGHT;
        auto& program=_objects->programs[color?0:i>=RN_GUI_TEXT?2:1];
        auto target=ColorTargetState::Default(format); target.Blend=translucent;
        if(i==RN_GUI_INVERT || i==RN_GUI_CROSSHAIR) target.Blend=BlendState{
            {BlendFactor::OneMinusDst,BlendFactor::OneMinusSrc,BlendOperation::Add},{BlendFactor::One,BlendFactor::Zero,BlendOperation::Add}};
        if(i==RN_GUI_HIGHLIGHT || i==RN_GUI_NAUSEA) target.Blend=BlendState{
            {BlendFactor::One,BlendFactor::One,BlendOperation::Add},{BlendFactor::One,BlendFactor::One,BlendOperation::Add}};
        if(i==RN_GUI_PREMULTIPLIED) target.Blend=BlendState{
            {BlendFactor::One,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add},{BlendFactor::One,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add}};
        if(i==RN_GUI_OPAQUE) target.Blend.reset();
        if(i==RN_GUI_VIGNETTE) target.Blend=BlendState{
            {BlendFactor::Zero,BlendFactor::OneMinusSrc,BlendOperation::Add},{BlendFactor::Zero,BlendFactor::One,BlendOperation::Add}};
        if(i==RN_GUI_LOGO) target.Blend=BlendState{
            {BlendFactor::SrcAlpha,BlendFactor::One,BlendOperation::Add},{BlendFactor::SrcAlpha,BlendFactor::One,BlendOperation::Add}};
        const VertexAttribute attributes[]{
            {0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},{0,20,"COLOR",0,VertexFormat::UNORM8X4,color?1u:2u},
            {0,12,"TEXCOORD",0,VertexFormat::FLOAT32X2,1}};
        pipelines[i]=device.CreateGraphicsPipelineState({.PipelineLayout=program.layout.get(),
            .VS=ShaderEntry{program.vs.get(),program.program->Entry(radray::shader::ShaderStage::Vertex)},
            .PS=ShaderEntry{program.ps.get(),program.program->Entry(radray::shader::ShaderStage::Pixel)},
            .VertexInput={{&vertex,1},{attributes,color?2u:3u}},.Primitive=primitive,.DepthStencil=std::nullopt,
            .MultiSample=MultiSampleState::Default(),.ColorTargets={&target,1}}).Release();
        if(!pipelines[i]) { RADRAY_ERR_LOG("Cannot create GUI pipeline variant"); return false; }
    }
    return true;
}
bool GuiPassResources::CreateSlot(Device& device,GuiFrameSlot& slot) {
    for(auto& parameters:slot.passParameters) {
        parameters=device.CreateShaderParameterSet({_objects->passLayout.get()}).Release();
        if(!parameters) { RADRAY_ERR_LOG("Cannot create GUI frame parameters"); return false; }
    }
    return true;
}
bool GuiPassResources::Prepare(Device& device,GuiFrameSlot& slot,const UploadedTextures& textures,const rn_render_frame_desc& f,uint32_t width,uint32_t height) {
    slot.draws.clear(); slot.vertexCount=0; slot.vertices={};
    if(f.gui_vertex_count>4194304 || f.gui_draw_count>1048576 || (f.gui_vertex_count && !f.gui_vertices) || (f.gui_draw_count && !f.gui_draws) || f.gui_vertex_count%4 ||
       ((f.gui_vertex_count || f.gui_draw_count) && (!std::isfinite(f.gui_width) || !std::isfinite(f.gui_height) || f.gui_width<=0 || f.gui_height<=0))) {
        RADRAY_ERR_LOG("Invalid GUI arrays or logical extent"); return false;
    }
    for(size_t i=f.gui_draw_count;i<slot.textureParameters.size();++i) {
        auto& binding=slot.textureParameters[i];
        if(binding.parameters) binding.parameters->ResetBindingsForReuse();
        binding.texture.Reset(); binding.sampler=nullptr;
    }
    if(!f.gui_draw_count) return true;
    slot.vertices=std::as_bytes(std::span{f.gui_vertices,f.gui_vertex_count});
    slot.width=f.gui_width; slot.height=f.gui_height;
    slot.yDirection=device.GetBackend()==RenderBackend::Vulkan?-1.f:1.f;
    while(slot.textureParameters.size()<f.gui_draw_count) slot.textureParameters.emplace_back();
    slot.draws.reserve(f.gui_draw_count);
    for(uint32_t i=0;i<f.gui_draw_count;++i) {
        const auto& desc=f.gui_draws[i];
        if(desc.pipeline>RN_GUI_TEXT_GRAYSCALE || desc.reserved || (desc.sampler_bits&~63u) || desc.first_vertex%4 || desc.vertex_count%4 ||
           uint64_t(desc.first_vertex)+desc.vertex_count>f.gui_vertex_count || desc.scissor[2]<0 || desc.scissor[3]<0) {
            RADRAY_ERR_LOG("Invalid GUI draw, pipeline, vertex range or scissor"); return false;
        }
        const double sx=double(width)/f.gui_width,sy=double(height)/f.gui_height;
        const auto left=uint32_t(std::clamp(std::floor(desc.scissor[0]*sx),0.,double(width)));
        const auto top=uint32_t(std::clamp(std::floor(desc.scissor[1]*sy),0.,double(height)));
        const auto right=uint32_t(std::clamp(std::ceil((int64_t(desc.scissor[0])+desc.scissor[2])*sx),0.,double(width)));
        const auto bottom=uint32_t(std::clamp(std::ceil((int64_t(desc.scissor[1])+desc.scissor[3])*sy),0.,double(height)));
        GuiDraw draw{.desc=desc,.scissor={int32_t(left),int32_t(top),right-left,bottom-top}};
        if(desc.pipeline>RN_GUI_HIGHLIGHT) {
            const auto* owner=textures.Find(desc.texture);
            if(!owner) { RADRAY_ERR_LOG("GUI draw={} references missing uploaded texture={}",i,desc.texture); return false; }
            draw.texture=owner->texture.Get();
            const uint64_t key=(uint64_t(owner->desc.mip_levels)<<32)|desc.sampler_bits;
            auto& sampler=_objects->samplers[key];
            if(!sampler) sampler=device.CreateSampler({.AddressS=(desc.sampler_bits&1)?AddressMode::Repeat:AddressMode::ClampToEdge,
                .AddressT=(desc.sampler_bits&2)?AddressMode::Repeat:AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
                .MinFilter=(desc.sampler_bits&4)?FilterMode::Linear:FilterMode::Nearest,.MagFilter=(desc.sampler_bits&8)?FilterMode::Linear:FilterMode::Nearest,
                .MipmapFilter=(desc.sampler_bits&16)?FilterMode::Linear:FilterMode::Nearest,.LodMax=(desc.sampler_bits&32)?0.f:float(owner->desc.mip_levels-1),.AnisotropyClamp=1}).Release();
            auto& binding=slot.textureParameters[i];
            if(!binding.parameters) binding.parameters=device.CreateShaderParameterSet({_objects->textureLayout.get()}).Release();
            if(!sampler || !binding.parameters) { RADRAY_ERR_LOG("Cannot create GUI texture bindings"); return false; }
            if(binding.texture.Get()!=draw.texture || binding.sampler!=sampler.get()) {
                binding.parameters->ResetBindingsForReuse();
                binding.texture=owner->texture; binding.sampler=sampler.get();
                if(!binding.parameters->Set(0,0,binding.texture->GetDefaultView()) ||
                   !binding.parameters->Set(1,0,binding.sampler) || !binding.parameters->FlushWrites()) {
                    RADRAY_ERR_LOG("Cannot prepare GUI texture bindings"); return false;
                }
            }
            draw.parameters=binding.parameters.get();
        } else {
            auto& binding=slot.textureParameters[i];
            if(binding.parameters) binding.parameters->ResetBindingsForReuse();
            binding.texture.Reset(); binding.sampler=nullptr;
        }
        slot.draws.push_back(std::move(draw));
    }
    slot.vertexCount=f.gui_vertex_count;
    return true;
}
void GuiPassResources::AddPass(radray::RDGBuilder& graph,radray::FrameSlot& slot,GuiFrameSlot& frame,
    radray::RDGTextureHandle color,uint32_t width,uint32_t height) {
    if(frame.draws.empty()) return;
    const auto vertices=graph.CreateBuffer({.MaxSize=frame.vertices.size(),.Usage=BufferUse::Vertex},radray::RDGUploadSource::BorrowedBytes(frame.vertices));
    const uint64_t indexBytes=uint64_t(frame.vertexCount/4)*24;
    const auto indices=graph.CreateBuffer({.MaxSize=indexBytes,.Usage=BufferUse::Index},
        radray::RDGUploadSource::Generated(indexBytes,frame.vertexCount,+[](const uint32_t& count,radray::RDGUploadWriteContext& context) noexcept {
            return WriteQuadIndices(context.Bytes,count);
        }));
    radray::array<radray::RDGBufferHandle,2> constants;
    for(uint32_t i=0;i<2;++i) {
        const radray::array<float,4> values{frame.width,frame.height,frame.yDirection,float(i)};
        constants[i]=graph.CreateBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,values,
            +[](const radray::array<float,4>& values,radray::RDGUploadWriteContext& context) noexcept {
                std::memset(context.Bytes.data(),0,context.Bytes.size()); std::memcpy(context.Bytes.data(),values.data(),sizeof(values)); return true;
            }));
    }
    struct PassData {
        radray::array<GraphicsPipelineState*,12> pipelines;
        GuiFrameSlot* frame;
        uint32_t width,height;
        radray::RDGBufferHandle vertices,indices;
        radray::array<radray::RDGBufferHandle,2> constants;
        radray::RDGBufferSlice vertexSlice{},indexSlice{};
    };
    radray::array<GraphicsPipelineState*,12> pipelines;
    for(size_t i=0;i<pipelines.size();++i) pipelines[i]=_objects->pipelines[i].get();
    auto pass=graph.AddRasterPass("Raynium GUI",PassData{pipelines,&frame,width,height,vertices,indices,constants},
        +[](const PassData& data,radray::RDGRasterPassContext& context) noexcept {
            try {
                auto& encoder=context.GetEncoder();
                encoder.SetViewport({0,0,float(data.width),float(data.height),0,1});
                return RecordGuiDraws(*data.frame,data.pipelines,data.vertexSlice,data.indexSlice,encoder);
            } catch(...) { AbortUnexpectedException("Raynium GUI record"); }
        },+[](PassData& data,radray::RDGPrepareContext& context) noexcept {
            data.vertexSlice=context.ResolveBuffer(data.vertices); data.indexSlice=context.ResolveBuffer(data.indices);
            for(uint32_t i=0;i<2;++i)
                if(!BindFrameConstants(*data.frame->passParameters[i],data.frame->boundConstants[i],context.ResolveBuffer(data.constants[i]))) return false;
            return true;
        });
    pass.ReadBuffer(vertices,BufferState::Vertex).ReadBuffer(indices,BufferState::Index)
        .ReadBuffer(constants[0],BufferState::CBuffer).ReadBuffer(constants[1],BufferState::CBuffer)
        .WriteColor(0,color,LoadAction::Load,StoreAction::Store);
    std::pmr::unordered_set<radray::GpuTexture*> textures{&slot.GetScratchMemoryResource()};
    for(const auto& draw:frame.draws) if(draw.texture && textures.insert(draw.texture).second)
        pass.ReadTexture(graph.ImportPersistentBorrowed(*draw.texture));
}
}
