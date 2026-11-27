#include <raynium/mesh_pass.h>

#include <algorithm>
#include <span>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/types.h>

namespace raynium {
namespace {
radray::unique_ptr<radray::gpu::GraphicsPipelineState> CreateTerrainPipeline(
    radray::gpu::Device& device,
    const ShaderProgram& program,
    radray::gpu::Shader* vs,
    radray::gpu::Shader* ps,
    radray::gpu::PipelineLayout* layout,
    uint32_t i,
    radray::gpu::TextureFormat format,
    bool wireframe = false,
    radray::Nullable<radray::gpu::PipelineCache*> cache = nullptr) {
    const radray::gpu::VertexBufferLayout layouts[]{
        {0, 28, radray::gpu::VertexStepMode::Vertex},
        {1, 16, radray::gpu::VertexStepMode::Instance},
    };
    const radray::gpu::VertexAttribute attributes[]{
        {0, 0, "POSITION", 0, radray::gpu::VertexFormat::FLOAT32X3, 0},
        {0, 12, "COLOR", 0, radray::gpu::VertexFormat::UNORM8X4, 1},
        {0, 16, "TEXCOORD", 0, radray::gpu::VertexFormat::FLOAT32X2, 2},
        {0, 24, "TEXCOORD", 1, radray::gpu::VertexFormat::UINT16X2, 3},
        {1, 0, "TEXCOORD", 2, radray::gpu::VertexFormat::SINT32X3, 4},
        {1, 12, "TEXCOORD", 3, radray::gpu::VertexFormat::UINT32, 5},
    };
    const radray::gpu::VertexAttribute alphaAttributes[]{
        attributes[0],
        attributes[1],
        attributes[2],
        {1, 0, "TEXCOORD", 2, radray::gpu::VertexFormat::SINT32X3, 3},
    };
    const bool alpha = i == OitBounds || i == OitTransmittance;
    auto primitive = radray::gpu::PrimitiveState::Default();
    primitive.Cull = radray::gpu::CullMode::Back;
    primitive.UnclippedDepth = false;
    primitive.FaceClockwise = radray::gpu::FrontFace::CW;
    primitive.Poly = wireframe ? radray::gpu::PolygonMode::Line : radray::gpu::PolygonMode::Fill;
    auto depth = radray::gpu::DepthStencilState::Default();
    depth.Format = radray::gpu::TextureFormat::D32_FLOAT;
    depth.DepthTestEnable = true;
    depth.DepthWriteEnable = i <= Translucent;
    depth.DepthCompare = radray::gpu::CompareFunction::GreaterEqual;
    radray::array<radray::gpu::ColorTargetState, 2> targets{
        radray::gpu::ColorTargetState::Default(format),
        radray::gpu::ColorTargetState::Default(format),
    };
    if (i == Translucent) {
        targets[0].Blend = radray::gpu::BlendState{
            {radray::gpu::BlendFactor::SrcAlpha, radray::gpu::BlendFactor::OneMinusSrcAlpha, radray::gpu::BlendOperation::Add},
            {radray::gpu::BlendFactor::One, radray::gpu::BlendFactor::OneMinusSrcAlpha, radray::gpu::BlendOperation::Add},
        };
    }
    if (i >= OitBounds && i <= OitAccumulate) {
        targets[0].Format = i == OitBounds ? radray::gpu::TextureFormat::RGBA32_FLOAT : radray::gpu::TextureFormat::RGBA16_FLOAT;
        const radray::gpu::BlendComponent add{
            radray::gpu::BlendFactor::One,
            radray::gpu::BlendFactor::One,
            i == OitBounds ? radray::gpu::BlendOperation::Max : radray::gpu::BlendOperation::Add,
        };
        targets[0].Blend = radray::gpu::BlendState{add, add};
        targets[1] = targets[0];
    }
    radray::gpu::VertexInputState input{};
    input = {
        layouts,
        alpha ? std::span<const radray::gpu::VertexAttribute>{alphaAttributes} : std::span<const radray::gpu::VertexAttribute>{attributes},
    };
    return device.CreateGraphicsPipelineState({
                                                  .PipelineLayout = layout,
                                                  .VS = radray::gpu::ShaderEntry{vs, program.Entry(radray::shader::ShaderStage::Vertex)},
                                                  .PS = radray::gpu::ShaderEntry{ps, program.Entry(radray::shader::ShaderStage::Pixel)},
                                                  .VertexInput = input,
                                                  .Primitive = primitive,
                                                  .DepthStencil = depth,
                                                  .MultiSample = radray::gpu::MultiSampleState::Default(),
                                                  .ColorTargets = {targets.data(), i == OitTransmittance ? 2u : 1u},
                                                  .Cache = cache,
                                              })
        .Release();
}
}  // namespace

MeshPassResources::MeshPassResources(radray::unique_ptr<Objects> objects)
    : _objects(std::move(objects)) {}

const radray::shared_ptr<radray::gpu::BindingGroupLayout>& MeshPassResources::PassGroupLayout() const noexcept {
    return _objects->PassGroupLayout;
}

radray::gpu::GraphicsPipelineState* MeshPassResources::Pipeline() const noexcept {
    return _objects->Programs[Solid].Pipeline.get();
}

bool MeshPassResources::SupportsWireframe() const noexcept {
    return bool(_objects->Wireframe);
}

radray::gpu::GraphicsPipelineState* MeshPassResources::WireframePipeline() const noexcept {
    return _objects->Wireframe.get();
}

radray::gpu::GraphicsPipelineState* MeshPassResources::Pipeline(uint32_t pass, bool wireframe) const noexcept {
    if (pass == Wireframe || (wireframe && pass <= Translucent)) return _objects->Wireframe.get();
    return _objects->Programs[pass].Pipeline.get();
}

const radray::shared_ptr<radray::gpu::BindingGroupLayout>& MeshPassResources::OitGroupLayout() const noexcept {
    return _objects->OitGroupLayout;
}

const radray::shared_ptr<radray::gpu::BindingGroupLayout>& MeshPassResources::MaterialGroupLayout() const noexcept {
    return _objects->MaterialGroupLayout;
}

bool MeshPassResources::Create(
    radray::gpu::Device& device,
    std::span<const radray::byte> shaders,
    radray::unique_ptr<MeshPassResources>& output,
    radray::Nullable<radray::gpu::PipelineCache*> cache) {
    RADRAY_PROFILE_SCOPE_N("Raynium.MeshPass.Create");
    if (output) RADRAY_ABORT("Pass resources output occupied");
    auto objects = radray::make_unique<Objects>();
    auto& p = *objects;
    const bool vk = device.GetBackend() == radray::gpu::GpuBackend::Vulkan;
    radray::vector<ShaderPackEntry> entries;
    if (!ReadShaderPack(shaders, entries)) return false;
    for (size_t i = 0; i < kTerrainPrograms.size(); ++i) {
        auto& program = p.Programs[i];
        if (!ShaderProgram::Create(entries, kTerrainPrograms[i], vk ? radray::shader::ShaderTarget::SPIRV : radray::shader::ShaderTarget::DXIL, program.Shader) ||
            !ValidateTerrainShader(program.Shader->Artifact(), kTerrainPrograms[i])) {
            return false;
        }
        const auto& artifact = program.Shader->Artifact();
        const auto blob = vk ? radray::gpu::ShaderBlobCategory::SPIRV : radray::gpu::ShaderBlobCategory::DXIL;
        program.Vs = device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex), blob, radray::gpu::ShaderStage::Vertex}).Release();
        program.Ps = device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel), blob, radray::gpu::ShaderStage::Pixel}).Release();
        if (!program.Vs || !program.Ps) {
            RADRAY_ERR_LOG("Cannot create terrain shader stages");
            return false;
        }
    }
    if (device.GetCapabilities().Limits.MaxColorAttachments < 2) {
        RADRAY_ERR_LOG("Terrain OIT requires two color attachments");
        return false;
    }
    for (auto format : {radray::gpu::TextureFormat::RGBA32_FLOAT, radray::gpu::TextureFormat::RGBA16_FLOAT}) {
        auto support = device.QueryTextureSupport({
            radray::gpu::TextureDimension::Dim2D,
            format,
            radray::gpu::TextureUse::RenderTarget | radray::gpu::TextureUse::Resource | radray::gpu::TextureUse::CopySource | radray::gpu::TextureUse::CopyDestination,
        });
        if (!support.Supported || !support.Blending) {
            RADRAY_ERR_LOG("Terrain OIT floating-point attachments or blending unsupported");
            return false;
        }
    }
    const radray::gpu::BindingDescriptor passBinding{
        0,
        radray::gpu::BindingKind::CBuffer,
        1,
        radray::gpu::ShaderStage::Vertex | radray::gpu::ShaderStage::Pixel,
        radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::CBV, 0, 0},
        radray::gpu::VulkanBindingDescriptor{0, 0},
    };
    const radray::gpu::BindingDescriptor materialBindings[]{
        {1, radray::gpu::BindingKind::Texture, 1, radray::gpu::ShaderStage::Pixel, radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::SRV, 0, 1}, radray::gpu::VulkanBindingDescriptor{1, 1}},
        {2, radray::gpu::BindingKind::Texture, 1, radray::gpu::ShaderStage::Vertex, radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::SRV, 1, 1}, radray::gpu::VulkanBindingDescriptor{1, 2}},
        {3, radray::gpu::BindingKind::Sampler, 1, radray::gpu::ShaderStage::Pixel, radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::Sampler, 0, 1}, radray::gpu::VulkanBindingDescriptor{1, 3}},
        {4, radray::gpu::BindingKind::Sampler, 1, radray::gpu::ShaderStage::Vertex, radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::Sampler, 1, 1}, radray::gpu::VulkanBindingDescriptor{1, 4}},
    };
    p.PassGroupLayout = device.CreateBindingGroupLayout({{&passBinding, 1}}).Release();
    p.MaterialGroupLayout = device.CreateBindingGroupLayout({materialBindings}).Release();
    radray::array<radray::gpu::BindingDescriptor, 4> oitBindings;
    for (uint32_t i = 0; i < 4; ++i) {
        oitBindings[i] = {
            i,
            radray::gpu::BindingKind::Texture,
            1,
            radray::gpu::ShaderStage::Pixel,
            radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::SRV, i, 2},
            radray::gpu::VulkanBindingDescriptor{2, i},
        };
    }
    p.OitGroupLayout = device.CreateBindingGroupLayout({oitBindings}).Release();
    if (!p.OitGroupLayout || !p.PassGroupLayout || !p.MaterialGroupLayout) {
        RADRAY_ERR_LOG("Cannot create SOLID shaders or bindings");
        return false;
    }
    for (uint32_t i = 0; i < kTerrainPrograms.size(); ++i) {
        auto& program = p.Programs[i];
        radray::vector<radray::gpu::PipelineLayoutGroupDescriptor> groups;
        groups.push_back({0, p.PassGroupLayout.get()});
        groups.push_back({1, p.MaterialGroupLayout.get()});
        if (i >= OitTransmittance) groups.push_back({2, p.OitGroupLayout.get()});
        program.PipelineLayout = device.CreatePipelineLayout({groups, {}}).Release();
        if (!program.PipelineLayout) {
            RADRAY_ERR_LOG("Cannot create terrain pipeline layout");
            return false;
        }
        program.Pipeline = CreateTerrainPipeline(
            device, *program.Shader, program.Vs.get(), program.Ps.get(), program.PipelineLayout.get(), i, radray::gpu::TextureFormat::RGBA8_UNORM, false, cache);
        if (!program.Pipeline) {
            RADRAY_ERR_LOG("Cannot create terrain pipeline: {}", kTerrainPrograms[i]);
            return false;
        }
    }
    const auto& solid = p.Programs[Solid];
    p.Wireframe = CreateTerrainPipeline(
        device, *solid.Shader, solid.Vs.get(), solid.Ps.get(), solid.PipelineLayout.get(), Solid, radray::gpu::TextureFormat::RGBA8_UNORM, true, cache);
    output = radray::unique_ptr<MeshPassResources>(new MeshPassResources(std::move(objects)));
    return true;
}
}  // namespace raynium
