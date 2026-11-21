#include <raynium/mesh_pass.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <algorithm>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::gpu;
namespace {
radray::unique_ptr<GraphicsPipelineState> CreateTerrainPipeline(Device& device,const ShaderProgram& program,
    Shader* vs,Shader* ps,PipelineLayout* layout,uint32_t i,TextureFormat format,bool wireframe=false,
    radray::Nullable<PipelineCache*> cache=nullptr) {
    const VertexBufferLayout layouts[]{{0,28,VertexStepMode::Vertex},{1,16,VertexStepMode::Instance}};
    const VertexAttribute attributes[]{
        {0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        {0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    const VertexAttribute alphaAttributes[]{attributes[0],attributes[1],attributes[2],
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,3}};
    const bool alpha=i==OitBounds || i==OitTransmittance;
    auto primitive=PrimitiveState::Default(); primitive.Cull=CullMode::Back; primitive.UnclippedDepth=false;
    primitive.FaceClockwise=FrontFace::CW;
    primitive.Poly=wireframe?PolygonMode::Line:PolygonMode::Fill;
    auto depth=DepthStencilState::Default(); depth.Format=TextureFormat::D32_FLOAT;
    depth.DepthTestEnable=true; depth.DepthWriteEnable=i<=Translucent;
    depth.DepthCompare=CompareFunction::GreaterEqual;
    radray::array<ColorTargetState,2> targets{ColorTargetState::Default(format),ColorTargetState::Default(format)};
    if(i==Translucent) targets[0].Blend=BlendState{
        {BlendFactor::SrcAlpha,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add},
        {BlendFactor::One,BlendFactor::OneMinusSrcAlpha,BlendOperation::Add}};
    if(i>=OitBounds && i<=OitAccumulate) {
        targets[0].Format=i==OitBounds?TextureFormat::RGBA32_FLOAT:TextureFormat::RGBA16_FLOAT;
        const BlendComponent add{BlendFactor::One,BlendFactor::One,i==OitBounds?BlendOperation::Max:BlendOperation::Add};
        targets[0].Blend=BlendState{add,add}; targets[1]=targets[0];
    }
    VertexInputState input{};
    input={layouts,alpha?std::span<const VertexAttribute>{alphaAttributes}:std::span<const VertexAttribute>{attributes}};
    return device.CreateGraphicsPipelineState({.PipelineLayout=layout,.VS=ShaderEntry{vs,program.Entry(radray::shader::ShaderStage::Vertex)},
        .PS=ShaderEntry{ps,program.Entry(radray::shader::ShaderStage::Pixel)},.VertexInput=input,.Primitive=primitive,.DepthStencil=depth,
        .MultiSample=MultiSampleState::Default(),.ColorTargets={targets.data(),i==OitTransmittance?2u:1u},.Cache=cache}).Release();
}
}
bool MeshPassResources::Create(Device& device,std::span<const radray::byte> shaders,radray::unique_ptr<MeshPassResources>& output,
    radray::Nullable<PipelineCache*> cache) {
    RADRAY_PROFILE_SCOPE_N("Raynium.MeshPass.Create");
    if(output) RADRAY_ABORT("Pass resources output occupied");
    auto objects=radray::make_unique<Objects>(); auto& p=*objects;
    const bool vk=device.GetBackend()==GpuBackend::Vulkan;
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
    for(uint32_t i=0;i<TerrainPrograms.size();++i) {
        auto& program=p.programs[i];
        radray::vector<PipelineLayoutGroupDescriptor> groups;
        groups.push_back({0,p.passGroupLayout.get()});
        groups.push_back({1,p.materialGroupLayout.get()});
        if(i>=OitTransmittance) groups.push_back({2,p.oitGroupLayout.get()});
        program.pipelineLayout=device.CreatePipelineLayout({groups,{}}).Release();
        if(!program.pipelineLayout) { RADRAY_ERR_LOG("Cannot create terrain pipeline layout"); return false; }
        program.pipeline=CreateTerrainPipeline(device,*program.program,program.vs.get(),program.ps.get(),program.pipelineLayout.get(),i,TextureFormat::RGBA8_UNORM,false,cache);
        if(!program.pipeline) { RADRAY_ERR_LOG("Cannot create terrain pipeline: {}",TerrainPrograms[i]); return false; }
    }
    const auto& solid=p.programs[Solid];
    p.wireframe=CreateTerrainPipeline(device,*solid.program,solid.vs.get(),solid.ps.get(),solid.pipelineLayout.get(),Solid,TextureFormat::RGBA8_UNORM,true,cache);
    output=radray::unique_ptr<MeshPassResources>(new MeshPassResources(std::move(objects))); return true;
}
bool MeshPassResources::PrepareOutput(Device& device,TextureFormat format) {
    if(format!=TextureFormat::RGBA8_UNORM || !device.QueryTextureSupport({TextureDimension::Dim2D,format,TextureUse::RenderTarget}).Supported) {
        RADRAY_ERR_LOG("RGBA8 scene output is unsupported"); return false;
    }
    return true;
}
}
