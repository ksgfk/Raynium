#include <raynium/mesh_pass.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <algorithm>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::render;
namespace {
radray::unique_ptr<GraphicsPipelineState> CreateTerrainPipeline(Device& device,const ShaderProgram& program,
    Shader* vs,Shader* ps,PipelineLayout* layout,uint32_t i,TextureFormat format,bool flipY=false) {
    const VertexBufferLayout layouts[]{{0,28,VertexStepMode::Vertex},{1,16,VertexStepMode::Instance}};
    const VertexAttribute attributes[]{
        {0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        {0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    const VertexAttribute alphaAttributes[]{attributes[0],attributes[1],attributes[2],
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,3}};
    const bool screen=i>=OitCull,alpha=i==OitBounds || i==OitTransmittance;
    auto primitive=PrimitiveState::Default(); primitive.Cull=screen?CullMode::None:CullMode::Back; primitive.UnclippedDepth=false;
    primitive.FaceClockwise=flipY?FrontFace::CCW:FrontFace::CW;
    auto depth=DepthStencilState::Default(); depth.Format=TextureFormat::D32_FLOAT;
    depth.DepthTestEnable=true; depth.DepthWriteEnable=i<=Translucent || screen;
    depth.DepthCompare=i==OitComposite?CompareFunction::Always:CompareFunction::GreaterEqual;
    radray::array<ColorTargetState,2> targets{ColorTargetState::Default(format),ColorTargetState::Default(format)};
    if(i==Translucent || i==OitComposite) targets[0].Blend=BlendState{
        {i==Translucent?BlendFactor::SrcAlpha:BlendFactor::One,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add},
        {BlendFactor::One,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add}};
    if(i>=OitBounds && i<=OitAccumulate) {
        targets[0].Format=i==OitBounds?TextureFormat::RGBA32_FLOAT:TextureFormat::RGBA16_FLOAT;
        const BlendComponent add{BlendFactor::One,BlendFactor::One,i==OitBounds?BlendOperation::Max:BlendOperation::Add};
        targets[0].Blend=BlendState{add,add}; targets[1]=targets[0];
    }
    if(i==OitCull) targets[0].Format=TextureFormat::RGBA32_FLOAT;
    VertexInputState input{};
    if(!screen) input={layouts,alpha?std::span<const VertexAttribute>{alphaAttributes}:std::span<const VertexAttribute>{attributes}};
    return device.CreateGraphicsPipelineState({.PipelineLayout=layout,.VS=ShaderEntry{vs,program.Entry(radray::shader::ShaderStage::Vertex)},
        .PS=ShaderEntry{ps,program.Entry(radray::shader::ShaderStage::Pixel)},.VertexInput=input,.Primitive=primitive,.DepthStencil=depth,
        .MultiSample=MultiSampleState::Default(),.ColorTargets={targets.data(),i==OitTransmittance?2u:1u}}).Release();
}
}
bool MeshPassResources::Create(Device& device,std::span<const radray::byte> shaders,radray::unique_ptr<MeshPassResources>& output) {
    RADRAY_PROFILE_SCOPE_N("Raynium.MeshPass.Create");
    if(output) RADRAY_ABORT("Pass resources output occupied");
    auto objects=radray::make_unique<Objects>(); auto& p=*objects;
    const bool vk=device.GetBackend()==RenderBackend::Vulkan;
    radray::vector<ShaderPackEntry> entries;
    if(!ReadShaderPack(shaders,entries)) return false;
    for(size_t i=0;i<TerrainPrograms.size();++i) {
        auto& program=p.programs[i];
        if(!ShaderProgram::Create(entries,TerrainPrograms[i],vk?radray::shader::ShaderTarget::SPIRV:radray::shader::ShaderTarget::DXIL,program.program) ||
           !ValidateTerrainShader(program.program->Artifact(),TerrainPrograms[i])) return false;
        const auto& artifact=program.program->Artifact();
        const auto blob=vk?ShaderBlobCategory::SPIRV:ShaderBlobCategory::DXIL;
        program.vs=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex),blob,ShaderStage::Vertex}).Release();
        program.ps=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel),blob,ShaderStage::Pixel}).Release();
        if(!program.vs || !program.ps) { RADRAY_ERR_LOG("Cannot create terrain shader stages"); return false; }
    }
    if(device.GetCapabilities().Limits.MaxColorAttachments<2) { RADRAY_ERR_LOG("Terrain OIT requires two color attachments"); return false; }
    for(auto format:{TextureFormat::RGBA32_FLOAT,TextureFormat::RGBA16_FLOAT}) {
        auto support=device.QueryTextureSupport({TextureDimension::Dim2D,format,TextureUse::RenderTarget|TextureUse::Resource|TextureUse::CopySource|TextureUse::CopyDestination});
        if(!support.Supported || !support.Blending) { RADRAY_ERR_LOG("Terrain OIT floating-point attachments or blending unsupported"); return false; }
    }
    const BindingDescriptor passBinding{0,BindingKind::CBuffer,1,ShaderStage::Vertex|ShaderStage::Pixel,
        D3D12BindingDescriptor{BindingRegisterClass::CBV,0,0},VulkanBindingDescriptor{0,0}};
    const BindingDescriptor materialBindings[]{
        {1,BindingKind::Texture,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::SRV,0,1},VulkanBindingDescriptor{1,1}},
        {2,BindingKind::Texture,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::SRV,1,1},VulkanBindingDescriptor{1,2}},
        {3,BindingKind::Sampler,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::Sampler,0,1},VulkanBindingDescriptor{1,3}},
        {4,BindingKind::Sampler,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::Sampler,1,1},VulkanBindingDescriptor{1,4}}};
    p.passGroupLayout=device.CreateBindingGroupLayout({{&passBinding,1}}).Release();
    p.materialGroupLayout=device.CreateBindingGroupLayout({materialBindings}).Release();
    radray::array<BindingDescriptor,4> oitBindings;
    for(uint32_t i=0;i<4;++i) oitBindings[i]={i,BindingKind::Texture,1,ShaderStage::Pixel,
        D3D12BindingDescriptor{BindingRegisterClass::SRV,i,2},VulkanBindingDescriptor{2,i}};
    p.oitGroupLayout=device.CreateBindingGroupLayout({oitBindings}).Release();
    if(!p.oitGroupLayout || !p.passGroupLayout || !p.materialGroupLayout) { RADRAY_ERR_LOG("Cannot create SOLID shaders or bindings"); return false; }
    for(uint32_t i=0;i<8;++i) {
        auto& program=p.programs[i];
        const bool screen=i>=OitCull;
        radray::vector<PipelineLayoutGroupDescriptor> groups;
        if(i!=OitComposite) groups.push_back({0,p.passGroupLayout.get()});
        if(!screen) groups.push_back({1,p.materialGroupLayout.get()});
        if(i>=OitTransmittance) groups.push_back({2,p.oitGroupLayout.get()});
        program.pipelineLayout=device.CreatePipelineLayout({groups,{}}).Release();
        if(!program.pipelineLayout) { RADRAY_ERR_LOG("Cannot create terrain pipeline layout"); return false; }
        program.pipeline=CreateTerrainPipeline(device,*program.program,program.vs.get(),program.ps.get(),program.pipelineLayout.get(),i,TextureFormat::BGRA8_UNORM);
        if(!program.pipeline) { RADRAY_ERR_LOG("Cannot create terrain pipeline: {}",TerrainPrograms[i]); return false; }
        if(!screen) {
            program.flippedPipeline=CreateTerrainPipeline(device,*program.program,program.vs.get(),program.ps.get(),program.pipelineLayout.get(),i,TextureFormat::BGRA8_UNORM,true);
            if(!program.flippedPipeline) { RADRAY_ERR_LOG("Cannot create reflected terrain pipeline"); return false; }
        }
    }
    output=radray::unique_ptr<MeshPassResources>(new MeshPassResources(std::move(objects))); return true;
}
bool MeshPassResources::PrepareOutput(Device& device,TextureFormat format) {
    if(format!=TextureFormat::BGRA8_UNORM || !device.QueryTextureSupport({TextureDimension::Dim2D,format,TextureUse::RenderTarget}).Supported) {
        RADRAY_ERR_LOG("BGRA8 output is unsupported"); return false;
    }
    return true;
}
bool MeshPassResources::CreateMaterialBinding(Device& device,const rn_render_frame_desc& desc,const UploadedTextures& uploaded,
    MeshMaterialBinding& binding) const {
    RADRAY_PROFILE_SCOPE_N("Raynium.Material.CreateBinding");
    const auto resolve=[&](uint32_t id,size_t index,TextureView*& view,uint32_t& mips) {
        if(id<16) { RADRAY_ERR_LOG("Material references reserved texture ID: {}",id); return false; }
        auto owner=uploaded.Find(id);
        if(!owner) { RADRAY_ERR_LOG("Material references missing uploaded texture: {}",id); return false; }
        view=owner->view.get(); mips=owner->desc.mip_levels; binding.textures[index]=std::move(owner); return true;
    };
    TextureView* albedo{}; TextureView* light{}; uint32_t albedoMips{},lightMips{};
    if(!resolve(desc.terrain_albedo_texture,0,albedo,albedoMips) || !resolve(desc.terrain_light_texture,1,light,lightMips)) return false;
    SamplerDescriptor sampler{.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
        .MinFilter=FilterMode::Linear,.MagFilter=FilterMode::Linear,.MipmapFilter=FilterMode::Linear,
        .LodMax=float(albedoMips-1),
        .AnisotropyClamp=std::clamp(desc.terrain_anisotropy,1u,uint32_t(device.GetCapabilities().Limits.MaxSamplerAnisotropy))};
    binding.albedoSampler=device.CreateSampler(sampler).Release();
    sampler.LodMax=float(lightMips-1); sampler.AnisotropyClamp=1;
    binding.lightSampler=device.CreateSampler(sampler).Release();
    binding.parameters=device.CreateShaderParameterSet({_objects->materialGroupLayout.get()}).Release();
    if(!binding.albedoSampler || !binding.lightSampler || !binding.parameters ||
       !binding.parameters->Set(1,0,albedo) || !binding.parameters->Set(2,0,light) ||
       !binding.parameters->Set(3,0,binding.albedoSampler.get()) || !binding.parameters->Set(4,0,binding.lightSampler.get()) ||
       !binding.parameters->FlushWrites()) { RADRAY_ERR_LOG("Cannot create SOLID material bindings"); return false; }
    return true;
}
}
