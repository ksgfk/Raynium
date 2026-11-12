#include <raynium/gui_pass.h>
#include <radray/types.h>
#include <raynium/exception.h>
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
    slot.constantStride=(256+device.GetCapabilities().Limits.CBufferOffsetAlignment-1)/device.GetCapabilities().Limits.CBufferOffsetAlignment*device.GetCapabilities().Limits.CBufferOffsetAlignment;
    slot.vertices=radray::MappedUploadBuffer::Create(device,64u<<10,BufferUse::Vertex).Release();
    slot.indices=radray::MappedUploadBuffer::Create(device,64u<<10,BufferUse::Index).Release();
    slot.constants=radray::MappedUploadBuffer::Create(device,slot.constantStride*2,BufferUse::CBuffer).Release();
    if(!slot.vertices || !slot.indices || !slot.constants) { RADRAY_ERR_LOG("Cannot create GUI slot uploads"); return false; }
    for(uint32_t i=0;i<2;++i) {
        auto& parameters=slot.passParameters[i];
        parameters=device.CreateShaderParameterSet({_objects->passLayout.get()}).Release();
        if(!parameters || !parameters->Set(0,0,ShaderBufferBinding{slot.constants->Get(),{i*slot.constantStride,256}}) || !parameters->FlushWrites()) {
            RADRAY_ERR_LOG("Cannot create GUI frame parameters"); return false;
        }
    }
    return true;
}
bool GuiPassResources::Prepare(Device& device,GuiFrameSlot& slot,const UploadedTextures& textures,const rn_render_frame_desc& f,uint32_t width,uint32_t height) {
    for(auto& parameters:slot.textureParameters) if(parameters) parameters->ResetBindingsForReuse();
    slot.draws.clear(); slot.vertexCount=0;
    if(f.gui_vertex_count>4194304 || f.gui_draw_count>1048576 || (f.gui_vertex_count && !f.gui_vertices) || (f.gui_draw_count && !f.gui_draws) || f.gui_vertex_count%4 ||
       ((f.gui_vertex_count || f.gui_draw_count) && (!std::isfinite(f.gui_width) || !std::isfinite(f.gui_height) || f.gui_width<=0 || f.gui_height<=0))) {
        RADRAY_ERR_LOG("Invalid GUI arrays or logical extent"); return false;
    }
    if(!f.gui_draw_count) return true;
    if(!slot.vertices->Reserve(device,uint64_t(f.gui_vertex_count)*24) || !slot.indices->Reserve(device,uint64_t(f.gui_vertex_count/4)*24)) {
        RADRAY_ERR_LOG("Cannot grow GUI vertices/indices"); return false;
    }
    std::memcpy(slot.vertices->GetData(),f.gui_vertices,size_t(f.gui_vertex_count)*24);
    auto* indices=reinterpret_cast<uint32_t*>(slot.indices->GetData());
    for(uint32_t v=0;v<f.gui_vertex_count;v+=4) {
        const uint32_t quad[]{v,v+1,v+2,v+2,v+3,v}; std::copy_n(quad,6,indices); indices+=6;
    }
    for(uint32_t i=0;i<2;++i) {
        const float viewport[]{f.gui_width,f.gui_height,device.GetBackend()==RenderBackend::Vulkan?-1.f:1.f,float(i)};
        std::memcpy(slot.constants->GetData()+i*slot.constantStride,viewport,sizeof(viewport));
    }
    if(!slot.vertices->Flush(0,uint64_t(f.gui_vertex_count)*24) || !slot.indices->Flush(0,uint64_t(f.gui_vertex_count/4)*24) ||
       !slot.constants->Flush(0,slot.constantStride*2)) { RADRAY_ERR_LOG("Cannot flush GUI frame buffers"); return false; }
    while(slot.textureParameters.size()<f.gui_draw_count) slot.textureParameters.push_back({});
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
            draw.texture=textures.Find(desc.texture);
            if(!draw.texture) { RADRAY_ERR_LOG("GUI draw={} references missing uploaded texture={}",i,desc.texture); return false; }
            const uint64_t key=(uint64_t(draw.texture->desc.mip_levels)<<32)|desc.sampler_bits;
            auto& sampler=_objects->samplers[key];
            if(!sampler) sampler=device.CreateSampler({.AddressS=(desc.sampler_bits&1)?AddressMode::Repeat:AddressMode::ClampToEdge,
                .AddressT=(desc.sampler_bits&2)?AddressMode::Repeat:AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
                .MinFilter=(desc.sampler_bits&4)?FilterMode::Linear:FilterMode::Nearest,.MagFilter=(desc.sampler_bits&8)?FilterMode::Linear:FilterMode::Nearest,
                .MipmapFilter=(desc.sampler_bits&16)?FilterMode::Linear:FilterMode::Nearest,.LodMax=(desc.sampler_bits&32)?0.f:float(draw.texture->desc.mip_levels-1),.AnisotropyClamp=1}).Release();
            auto& parameters=slot.textureParameters[i];
            if(!parameters) parameters=device.CreateShaderParameterSet({_objects->textureLayout.get()}).Release();
            if(!sampler || !parameters || !parameters->Set(0,0,draw.texture->view.get()) || !parameters->Set(1,0,sampler.get()) || !parameters->FlushWrites()) {
                RADRAY_ERR_LOG("Cannot prepare GUI texture bindings"); return false;
            }
            draw.parameters=parameters.get();
        }
        slot.draws.push_back(std::move(draw));
    }
    slot.vertexCount=f.gui_vertex_count;
    return true;
}
void GuiPassResources::AddPass(radray::RDGBuilder& graph,GuiFrameSlot& frame,radray::RDGTextureHandle color,TextureFormat format,uint32_t width,uint32_t height,
    std::span<const radray::RDGTextureHandle> textures) {
    if(frame.draws.empty()) return;
    const auto vertices=graph.ImportBuffer(frame.vertices->Get(),BufferState::HostWrite,BufferState::HostWrite);
    const auto indices=graph.ImportBuffer(frame.indices->Get(),BufferState::HostWrite,BufferState::HostWrite);
    const auto constants=graph.ImportBuffer(frame.constants->Get(),BufferState::HostWrite,BufferState::HostWrite);
    graph.AddRasterPass("Raynium GUI",[&](radray::RDGRasterPassBuilder& pass) {
        pass.ReadBuffer(vertices,BufferState::Vertex).ReadBuffer(indices,BufferState::Index).ReadBuffer(constants,BufferState::CBuffer)
            .WriteColor(0,color,LoadAction::Load,StoreAction::Store);
        for(auto texture:textures) pass.ReadTexture(texture);
    },[this,&frame,format,width,height](radray::RDGRasterPassContext& context) {
        try {
            auto& encoder=context.GetEncoder();
            encoder.SetViewport({0,0,float(width),float(height),0,1});
            const VertexBufferBinding vertices{0,{frame.vertices->Get(),0,uint64_t(frame.vertexCount)*24}};
            for(const auto& draw:frame.draws) {
                if(!draw.desc.vertex_count || !draw.scissor.Width || !draw.scissor.Height) continue;
                encoder.SetScissor(draw.scissor);
                encoder.BindGraphicsPipelineState(_objects->pipelines[draw.desc.pipeline].get());
                encoder.BindVertexBuffers({&vertices,1}); encoder.BindIndexBuffer({frame.indices->Get(),0,4});
                encoder.BindShaderParameterSet(0,frame.passParameters[draw.desc.pipeline==RN_GUI_TEXT_GRAYSCALE?1:0].get());
                if(draw.parameters) encoder.BindShaderParameterSet(1,draw.parameters);
                encoder.DrawIndexed(draw.desc.vertex_count/4*6,1,draw.desc.first_vertex/4*6,0,0);
            }
            if(!encoder.IsValid()) RADRAY_ABORT("Invalid GUI render encoder");
            return true;
        } catch(...) {
            AbortUnexpectedException("Raynium GUI record");
        }
        });
}
}
