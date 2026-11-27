#include <raynium/output_pass.h>

#include <cstring>
#include <utility>

#include <radray/logger.h>
#include <raynium/frame_upload.h>

namespace raynium {
OutputPassResources::OutputPassResources(radray::unique_ptr<Objects> objects)
    : _objects(std::move(objects)) {}

bool OutputPassResources::Create(
    radray::gpu::Device& device,
    std::span<const radray::byte> shaders,
    radray::unique_ptr<OutputPassResources>& out,
    radray::Nullable<radray::gpu::PipelineCache*> cache) {
    if (out) RADRAY_ABORT("Output pass resource output occupied");
    auto objects = radray::make_unique<Objects>();
    auto& p = *objects;
    const bool vk = device.GetBackend() == radray::gpu::GpuBackend::Vulkan;
    if (!ShaderProgram::Create(shaders, "output", vk ? radray::shader::ShaderTarget::SPIRV : radray::shader::ShaderTarget::DXIL, p.Program)) return false;
    const auto& artifact = p.Program->Artifact();
    if (!ValidateOutputShader(artifact)) return false;
    const radray::gpu::BindingDescriptor bindings[]{
        {0, radray::gpu::BindingKind::Texture, 1, radray::gpu::ShaderStage::Pixel, radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::SRV, 0, 0}, radray::gpu::VulkanBindingDescriptor{0, 0}},
        {1, radray::gpu::BindingKind::CBuffer, 1, radray::gpu::ShaderStage::Pixel, radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::CBV, 1, 0}, radray::gpu::VulkanBindingDescriptor{0, 1}},
    };
    p.Bindings = device.CreateBindingGroupLayout({bindings}).Release();
    if (!p.Bindings) return false;
    const radray::gpu::PipelineLayoutGroupDescriptor group{0, p.Bindings.get()};
    p.Layout = device.CreatePipelineLayout({{&group, 1}, {}}).Release();
    const auto blob = vk ? radray::gpu::ShaderBlobCategory::SPIRV : radray::gpu::ShaderBlobCategory::DXIL;
    p.Vs = device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex), blob, radray::gpu::ShaderStage::Vertex}).Release();
    p.Ps = device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel), blob, radray::gpu::ShaderStage::Pixel}).Release();
    if (!p.Layout || !p.Vs || !p.Ps) return false;
    auto primitive = radray::gpu::PrimitiveState::Default();
    primitive.Cull = radray::gpu::CullMode::None;
    const auto target = radray::gpu::ColorTargetState::Default(radray::gpu::TextureFormat::BGRA8_UNORM);
    p.Pipeline = device.CreateGraphicsPipelineState({
                                                        .PipelineLayout = p.Layout.get(),
                                                        .VS = radray::gpu::ShaderEntry{p.Vs.get(), p.Program->Entry(radray::shader::ShaderStage::Vertex)},
                                                        .PS = radray::gpu::ShaderEntry{p.Ps.get(), p.Program->Entry(radray::shader::ShaderStage::Pixel)},
                                                        .Primitive = primitive,
                                                        .MultiSample = radray::gpu::MultiSampleState::Default(),
                                                        .ColorTargets = {&target, 1},
                                                        .Cache = cache,
                                                    })
                     .Release();
    if (!p.Pipeline) return false;
    out = radray::unique_ptr<OutputPassResources>{new OutputPassResources(std::move(objects))};
    return true;
}

void OutputPassResources::AddPass(
    radray::RDGBuilder& graph,
    radray::RDGTextureHandle source,
    radray::RDGTextureHandle destination,
    uint32_t width,
    uint32_t height,
    uint32_t sourceMip,
    bool flip) const {
    const auto constants = graph.CreateDynamicBuffer(
        {.MaxSize = 256, .Usage = radray::gpu::BufferUse::CBuffer},
        radray::RDGUploadSource::Generated(
            256,
            flip ? height : 0,
            +[](const uint32_t& flippedHeight, radray::RDGUploadWriteContext& ctx) noexcept {
                std::memset(ctx.Bytes.data(), 0, ctx.Bytes.size());
                std::memcpy(ctx.Bytes.data(), &flippedHeight, sizeof(flippedHeight));
                return true;
            }));
    struct Data {
        const Objects* PassObjects;
        radray::RDGTextureHandle Source;
        uint32_t Width;
        uint32_t Height;
        uint32_t Mip;
        radray::RDGBufferHandle Constants;
        radray::gpu::ShaderParameterSet* Parameters{};
    };
    graph.AddRasterPass(
             "Raynium Output",
             Data{_objects.get(), source, width, height, sourceMip, constants},
             +[](const Data& data, radray::RDGRasterPassContext& context) noexcept {
                 auto& encoder = context.GetEncoder();
                 encoder.SetViewport({0, 0, float(data.Width), float(data.Height), 0, 1});
                 encoder.SetScissor({0, 0, data.Width, data.Height});
                 encoder.BindGraphicsPipelineState(data.PassObjects->Pipeline.get());
                 encoder.BindShaderParameterSet(0, data.Parameters);
                 encoder.Draw(3, 1, 0, 0);
                 return encoder.IsValid();
             },
             +[](Data& data, radray::RDGPrepareContext& context) noexcept {
                 auto view = context.ResolveView(data.Source, {
                                                                  .Dim = radray::gpu::TextureDimension::Dim2D,
                                                                  .Range = {0, 1, data.Mip, 1, radray::gpu::TextureAspect::Color},
                                                                  .Usage = radray::gpu::TextureViewUsage::Resource,
                                                              });
                 if (!view) return false;
                 const radray::TransientParameterBinding bindings[]{
                     {0, 0, view.Get()},
                     FrameConstantBinding(1, context.ResolveBuffer(data.Constants)),
                 };
                 data.Parameters = context.GetSlot().AcquireParameterSet(data.PassObjects->Bindings, bindings).Get();
                 return data.Parameters != nullptr;
             })
        .ReadBuffer(constants, radray::gpu::BufferState::CBuffer)
        .ReadTexture(source)
        .WriteColor(0, destination, radray::gpu::LoadAction::DontCare, radray::gpu::StoreAction::Store);
}
}  // namespace raynium
