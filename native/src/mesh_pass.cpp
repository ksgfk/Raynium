#include <raynium/mesh_pass.h>
#include <raynium/uv_bytecode.h>
#include <raynium/terrain_bytecode.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::render;
bool MeshPassResources::CreateProgram(Device& d,uint32_t id,MeshProgram& p,radray::string& error) {
    const bool textured=id==RN_PROGRAM_TEXTURED, vk=d.GetBackend()==RenderBackend::Vulkan;
    const auto blob=vk ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
    p.vs=d.CreateShader({textured ? (vk ? TerrainVsSpirv() : TerrainVsDxil()) : (vk ? UvVsSpirv() : UvVsDxil()),blob,ShaderStage::Vertex}).Unwrap();
    p.ps=d.CreateShader({textured ? (vk ? TerrainPsSpirv() : TerrainPsDxil()) : (vk ? UvPsSpirv() : UvPsDxil()),blob,ShaderStage::Pixel}).Unwrap();
    const auto stages=ShaderStage::Vertex|ShaderStage::Pixel;
    const BindingDescriptor bindings[]{
        {0,BindingKind::CBuffer,1,stages,D3D12BindingDescriptor{BindingRegisterClass::CBV,0,0},VulkanBindingDescriptor{0,0}},
        {1,BindingKind::Texture,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::SRV,0,0},VulkanBindingDescriptor{0,1}},
        {2,BindingKind::Texture,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::SRV,1,0},VulkanBindingDescriptor{0,2}},
        {3,BindingKind::Sampler,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::Sampler,0,0},VulkanBindingDescriptor{0,3}},
        {4,BindingKind::Sampler,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::Sampler,1,0},VulkanBindingDescriptor{0,4}}};
    p.group=d.CreateBindingGroupLayout({{bindings,textured ? 5u : 1u}}).Unwrap();
    if(!p.group || !p.vs || !p.ps) { error="Cannot create mesh program shaders or bindings"; return false; }
    const PipelineLayoutGroupDescriptor group{0,p.group.get()}; p.layout=d.CreatePipelineLayout({{&group,1},{}}).Unwrap();
    if(!p.layout) { error="Cannot create mesh program layout"; return false; }
    const VertexBufferLayout layouts[]{{0,28,VertexStepMode::Vertex},{1,16,VertexStepMode::Instance}};
    const VertexAttribute attributes[]{
        {0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        {0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    auto primitive=PrimitiveState::Default(); primitive.UnclippedDepth=false;
    auto depth=DepthStencilState::Default(); depth.DepthCompare=CompareFunction::GreaterEqual;
    if(!textured) { primitive.Cull=CullMode::None; depth.DepthTestEnable=false; depth.DepthWriteEnable=false; }
    const auto color=ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    const GraphicsPipelineStateDescriptor descriptor{.PipelineLayout=p.layout.get(),.VS=ShaderEntry{p.vs.get(),"VSMain"},.PS=ShaderEntry{p.ps.get(),"PSMain"},
        .VertexInput=textured ? VertexInputState{layouts,attributes} : VertexInputState{},.Primitive=primitive,.DepthStencil=depth,
        .MultiSample=MultiSampleState::Default(),.ColorTargets={&color,1}};
    p.cache=std::make_unique<radray::GraphicsPipelineCache>(d); std::string failure;
    p.pipeline=p.cache->Get(descriptor,GetAttachmentSignature(descriptor),failure).Get();
    p.vertexInput=textured; p.textures=textured;
    if(!p.pipeline) { error="Cannot create mesh program pipeline"; return false; }
    return true;
}
bool MeshPassResources::Create(Device& device,radray::unique_ptr<MeshPassResources>& output,radray::string& error) {
    if(output) { error="Pass resources output occupied"; return false; }
    std::array<MeshProgram,2> programs;
    if(!CreateProgram(device,RN_PROGRAM_UV,programs[0],error) || !CreateProgram(device,RN_PROGRAM_TEXTURED,programs[1],error)) return false;
    output=radray::unique_ptr<MeshPassResources>(new MeshPassResources(std::move(programs))); return true;
}
bool MeshPassResources::PrepareBinding(Device& d,Buffer& constants,std::map<uint64_t,MeshMaterialBinding>& materials,
    const radray::MeshDrawCommand& command,SharedTextures* textures,radray::string& error) {
    const auto& desc=command.bindings;
    uint32_t anisotropy=1; if(desc.parameters.size()==sizeof(anisotropy)) std::memcpy(&anisotropy,desc.parameters.data(),sizeof(anisotropy)); const auto& program=Program(command.program);
    const uint64_t tv=program.textures ? command.textureVersion : 0;
    auto old=materials.find(desc.id);
    if(old!=materials.end() && old->second.version==command.materialVersion && old->second.textureVersion==tv) return true;
    MeshMaterialBinding binding; binding.version=command.materialVersion; binding.textureVersion=tv;
    binding.parameters=d.CreateShaderParameterSet({program.group.get()}).Unwrap();
    if(!binding.parameters || !binding.parameters->Set(0,0,ShaderBufferBinding{&constants,{0,256}})) {
        error="Cannot create frame material bindings"; return false;
    }
    if(program.textures) {
        SamplerDescriptor sampler{.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
            .MinFilter=FilterMode::Linear,.MagFilter=FilterMode::Linear,.MipmapFilter=FilterMode::Linear,
            .LodMax=float(textures->Mips(desc.resources.at(0))-1),
            .AnisotropyClamp=std::clamp(anisotropy,1u,uint32_t(d.GetCapabilities().Limits.MaxSamplerAnisotropy))};
        binding.albedoSampler=d.CreateSampler(sampler).Unwrap(); sampler.LodMax=float(textures->Mips(desc.resources.at(1))-1); sampler.AnisotropyClamp=1;
        binding.lightSampler=d.CreateSampler(sampler).Unwrap();
        if(!binding.albedoSampler || !binding.lightSampler ||
           !binding.parameters->Set(1,0,textures->View(desc.resources.at(0))) || !binding.parameters->Set(2,0,textures->View(desc.resources.at(1))) ||
           !binding.parameters->Set(3,0,binding.albedoSampler.get()) || !binding.parameters->Set(4,0,binding.lightSampler.get())) {
            error="Cannot create material texture bindings"; return false;
        }
    }
    if(!binding.parameters->FlushWrites()) { error="Cannot flush material bindings"; return false; }
    materials.insert_or_assign(desc.id,std::move(binding)); return true;
}
}
