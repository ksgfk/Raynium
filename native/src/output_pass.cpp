#include <raynium/output_pass.h>
#include <raynium/exception.h>
#include <radray/logger.h>

namespace raynium {
using namespace radray::gpu;
bool OutputPassResources::Create(Device& device,std::span<const radray::byte> shaders,radray::unique_ptr<OutputPassResources>& out) {
    if(out) RADRAY_ABORT("Output pass resource output occupied");
    auto objects=radray::make_unique<Objects>(); auto& p=*objects;
    const bool vk=device.GetBackend()==GpuBackend::Vulkan;
    if(!ShaderProgram::Create(shaders,"output",vk?radray::shader::ShaderTarget::SPIRV:radray::shader::ShaderTarget::DXIL,p.program)) return false;
    const auto& artifact=p.program->Artifact();
    if(!ValidateOutputShader(artifact)) return false;
    const BindingDescriptor binding{0,BindingKind::Texture,1,ShaderStage::Pixel,
        D3D12BindingDescriptor{BindingRegisterClass::SRV,0,0},VulkanBindingDescriptor{0,0}};
    p.bindings=device.CreateBindingGroupLayout({{&binding,1}}).Release();
    if(!p.bindings) return false;
    const PipelineLayoutGroupDescriptor group{0,p.bindings.get()};
    p.layout=device.CreatePipelineLayout({{&group,1},{}}).Release();
    const auto blob=vk?ShaderBlobCategory::SPIRV:ShaderBlobCategory::DXIL;
    p.vs=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex),blob,ShaderStage::Vertex}).Release();
    p.ps=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel),blob,ShaderStage::Pixel}).Release();
    if(!p.layout || !p.vs || !p.ps) return false;
    auto primitive=PrimitiveState::Default(); primitive.Cull=CullMode::None;
    const auto target=ColorTargetState::Default(TextureFormat::BGRA8_UNORM);
    p.pipeline=device.CreateGraphicsPipelineState({.PipelineLayout=p.layout.get(),
        .VS=ShaderEntry{p.vs.get(),p.program->Entry(radray::shader::ShaderStage::Vertex)},
        .PS=ShaderEntry{p.ps.get(),p.program->Entry(radray::shader::ShaderStage::Pixel)},.Primitive=primitive,
        .MultiSample=MultiSampleState::Default(),.ColorTargets={&target,1}}).Release();
    if(!p.pipeline) return false;
    out=radray::unique_ptr<OutputPassResources>{new OutputPassResources(std::move(objects))}; return true;
}
void OutputPassResources::AddPass(radray::RDGBuilder& graph,radray::RDGTextureHandle source,
    radray::RDGTextureHandle destination,uint32_t width,uint32_t height) const {
    struct Data { const Objects* objects; radray::RDGTextureHandle source; uint32_t width,height; ShaderParameterSet* parameters{}; };
    graph.AddRasterPass("Raynium Output",Data{_objects.get(),source,width,height},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            auto& encoder=context.GetEncoder();
            encoder.SetViewport({0,0,float(data.width),float(data.height),0,1}); encoder.SetScissor({0,0,data.width,data.height});
            encoder.BindGraphicsPipelineState(data.objects->pipeline.get()); encoder.BindShaderParameterSet(0,data.parameters);
            encoder.Draw(3,1,0,0); return encoder.IsValid();
        },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
            auto view=context.ResolveView(data.source,TextureViewUsage::Resource);
            if(!view) return false;
            const radray::TransientParameterBinding binding{0,0,view.Get()};
            data.parameters=context.GetSlot().AcquireParameterSet(data.objects->bindings,{&binding,1}).Get();
            return data.parameters!=nullptr;
        }).ReadTexture(source).WriteColor(0,destination,LoadAction::DontCare,StoreAction::Store);
}
}
