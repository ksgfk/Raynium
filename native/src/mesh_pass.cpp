#include <raynium/mesh_pass.h>
#include <raynium/terrain_bytecode.h>
#include <algorithm>

namespace raynium {
using namespace radray::render;
bool MeshPassResources::Create(Device& device,radray::unique_ptr<MeshPassResources>& output,radray::string& error) {
    if(output) { error="Pass resources output occupied"; return false; }
    auto objects=radray::make_unique<Objects>(); auto& p=*objects;
    const bool vk=device.GetBackend()==RenderBackend::Vulkan;
    const auto blob=vk ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
    p.vs=device.CreateShader({vk ? TerrainVsSpirv() : TerrainVsDxil(),blob,ShaderStage::Vertex}).Release();
    p.ps=device.CreateShader({vk ? TerrainPsSpirv() : TerrainPsDxil(),blob,ShaderStage::Pixel}).Release();
    const BindingDescriptor passBinding{0,BindingKind::CBuffer,1,ShaderStage::Vertex|ShaderStage::Pixel,
        D3D12BindingDescriptor{BindingRegisterClass::CBV,0,0},VulkanBindingDescriptor{0,0}};
    const BindingDescriptor materialBindings[]{
        {1,BindingKind::Texture,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::SRV,0,1},VulkanBindingDescriptor{1,1}},
        {2,BindingKind::Texture,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::SRV,1,1},VulkanBindingDescriptor{1,2}},
        {3,BindingKind::Sampler,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::Sampler,0,1},VulkanBindingDescriptor{1,3}},
        {4,BindingKind::Sampler,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::Sampler,1,1},VulkanBindingDescriptor{1,4}}};
    p.passGroupLayout=device.CreateBindingGroupLayout({{&passBinding,1}}).Release();
    p.materialGroupLayout=device.CreateBindingGroupLayout({materialBindings}).Release();
    if(!p.vs || !p.ps || !p.passGroupLayout || !p.materialGroupLayout) { error="Cannot create SOLID shaders or bindings"; return false; }
    const PipelineLayoutGroupDescriptor groups[]{{0,p.passGroupLayout.get()},{1,p.materialGroupLayout.get()}};
    p.pipelineLayout=device.CreatePipelineLayout({groups,{}}).Release();
    if(!p.pipelineLayout) { error="Cannot create SOLID pipeline layout"; return false; }
    const VertexBufferLayout layouts[]{{0,28,VertexStepMode::Vertex},{1,16,VertexStepMode::Instance}};
    const VertexAttribute attributes[]{
        {0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        {0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    auto primitive=PrimitiveState::Default(); primitive.Cull=CullMode::Back; primitive.UnclippedDepth=false;
    auto depth=DepthStencilState::Default(); depth.Format=TextureFormat::D32_FLOAT; depth.DepthCompare=CompareFunction::GreaterEqual;
    depth.DepthTestEnable=true; depth.DepthWriteEnable=true;
    const auto color=ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    p.pipeline=device.CreateGraphicsPipelineState({.PipelineLayout=p.pipelineLayout.get(),.VS=ShaderEntry{p.vs.get(),"VSMain"},
        .PS=ShaderEntry{p.ps.get(),"PSMain"},.VertexInput={layouts,attributes},.Primitive=primitive,.DepthStencil=depth,
        .MultiSample=MultiSampleState::Default(),.ColorTargets={&color,1}}).Release();
    if(!p.pipeline) { error="Cannot create SOLID pipeline"; return false; }
    p.materialResource.Programs={{"Solid",p.pipelineLayout.get(),p.vs.get(),"VSMain",p.ps.get(),"PSMain"}};
    p.materialResource.Properties.RenderState.Cull=primitive.Cull;
    p.materialResource.Properties.RenderState.DepthCompare=depth.DepthCompare;
    p.materialResource.Properties.RenderState.DepthTestEnable=true;
    p.materialResource.Properties.RenderState.DepthWriteEnable=true;
    p.baseMaterial=radray::Material::Create(p.materialResource,{});
    if(!p.baseMaterial) { error="Cannot create SOLID material"; return false; }
    output=radray::unique_ptr<MeshPassResources>(new MeshPassResources(std::move(objects))); return true;
}
bool MeshPassResources::CreateMaterialBinding(Device& device,const rn_material_update& desc,const SharedTextures& textures,
    MeshMaterialBinding& binding,radray::MaterialRenderProxy& proxy,radray::string& error) const {
    auto* albedo=textures.View(desc.albedo_texture); auto* light=textures.View(desc.light_texture);
    SamplerDescriptor sampler{.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
        .MinFilter=FilterMode::Linear,.MagFilter=FilterMode::Linear,.MipmapFilter=FilterMode::Linear,
        .LodMax=float(textures.Mips(desc.albedo_texture)-1),
        .AnisotropyClamp=std::clamp(desc.anisotropy,1u,uint32_t(device.GetCapabilities().Limits.MaxSamplerAnisotropy))};
    binding.albedoSampler=device.CreateSampler(sampler).Release();
    sampler.LodMax=float(textures.Mips(desc.light_texture)-1); sampler.AnisotropyClamp=1;
    binding.lightSampler=device.CreateSampler(sampler).Release();
    binding.parameters=device.CreateShaderParameterSet({_objects->materialGroupLayout.get()}).Release();
    if(!binding.albedoSampler || !binding.lightSampler || !binding.parameters ||
       !binding.parameters->Set(1,0,albedo) || !binding.parameters->Set(2,0,light) ||
       !binding.parameters->Set(3,0,binding.albedoSampler.get()) || !binding.parameters->Set(4,0,binding.lightSampler.get()) ||
       !binding.parameters->FlushWrites()) { error="Cannot create SOLID material bindings"; return false; }
    proxy.Update(*_objects->baseMaterial); return true;
}
}
