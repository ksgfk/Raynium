#include <raynium/render_program.h>

#include <algorithm>
#include <utility>

#include <radray/logger.h>

namespace raynium {
namespace {
radray::gpu::ShaderStages ToShaderStages(uint32_t mask) {
    radray::gpu::ShaderStages stages{};
    if (mask & (1u << uint8_t(radray::shader::ShaderStage::Vertex))) stages |= radray::gpu::ShaderStage::Vertex;
    if (mask & (1u << uint8_t(radray::shader::ShaderStage::Pixel))) stages |= radray::gpu::ShaderStage::Pixel;
    return stages;
}
}  // namespace

bool RasterProgram::RequiresBinding(uint32_t group, uint32_t slot) const noexcept {
    if (slot < 64u) return (RequiredMask[group] & (uint64_t{1} << slot)) != 0;
    if (!HasHighBindings) return false;
    const auto& used = Required[group];
    return std::find(used.begin(), used.end(), slot) != used.end();
}

bool RasterProgram::HasSampler(uint32_t slot) const noexcept {
    if (slot < 64u) return (SamplerMask & (uint64_t{1} << slot)) != 0;
    if (!HasHighBindings) return false;
    return std::find(Samplers.begin(), Samplers.end(), slot) != Samplers.end();
}

RenderProgramLibrary::RenderProgramLibrary(radray::vector<radray::byte> bytes)
    : _bytes(std::move(bytes)) {}

bool RenderProgramLibrary::Create(std::span<const radray::byte> bytes, radray::unique_ptr<RenderProgramLibrary>& result) {
    if (result) return false;
    auto owner = radray::unique_ptr<RenderProgramLibrary>(new RenderProgramLibrary({bytes.begin(), bytes.end()}));
    if (!ReadShaderPack(owner->_bytes, owner->_entries)) return false;
    result = std::move(owner);
    return true;
}

RasterProgram* RenderProgramLibrary::GetProgram(
    radray::gpu::Device& device,
    std::string_view name,
    std::span<const radray::byte> external) {
    const bool vk = device.GetBackend() == radray::gpu::GpuBackend::Vulkan;
    auto& cached = _programs[radray::string(name)];
    if (!cached) {
        auto program = radray::make_unique<RasterProgram>();
        const auto lane = vk ? radray::shader::ShaderTarget::SPIRV : radray::shader::ShaderTarget::DXIL;
        if (external.empty()) {
            if (!ShaderProgram::Create(_entries, name, lane, program->Program)) return nullptr;
        } else {
            program->ExternalBytes.assign(external.begin(), external.end());
            radray::vector<ShaderPackEntry> entries;
            if (!ReadShaderPack(program->ExternalBytes, entries) || entries.size() != 2 ||
                entries[0].Name != name || entries[1].Name != name || !ShaderProgram::Create(entries, name, lane, program->Program)) {
                return nullptr;
            }
        }
        const auto& artifact = program->Program->Artifact();
        if (artifact.RootConstants().size() > 1 || !artifact.Samplers().empty()) return nullptr;
        radray::vector<radray::gpu::PushConstantDescriptor> push;
        for (const auto& root : artifact.RootConstants()) {
            if (!vk || root.Offset || !root.Size || root.Size > device.GetCapabilities().Limits.MaxPushConstantBytes) return nullptr;
            program->PushBytes = root.Size;
            push.push_back({0, root.Size, ToShaderStages(root.StageMask), {}, radray::gpu::PushConstantDescriptor::VulkanLocation{0}});
        }
        radray::array<radray::vector<radray::gpu::BindingDescriptor>, 2> bindings;
        for (const auto& binding : artifact.Bindings()) {
            const auto kind = radray::shader::DecodeBindingKind(binding.Type);
            if (!vk && binding.Group == 3 && binding.Binding == 0 && kind == radray::gpu::BindingKind::CBuffer && binding.Count == 1) {
                const radray::gpu::BindingDescriptor desc{
                    0,
                    radray::gpu::BindingKind::CBuffer,
                    1,
                    radray::gpu::ShaderStage::Vertex,
                    radray::gpu::D3D12BindingDescriptor{radray::gpu::BindingRegisterClass::CBV, 0, 3},
                    {},
                };
                program->DrawBasesLayout = device.CreateBindingGroupLayout({{&desc, 1}}).Release();
                if (!program->DrawBasesLayout) return nullptr;
                continue;
            }
            if (!vk && binding.Group == 2 && binding.Binding == 0 && kind == radray::gpu::BindingKind::CBuffer && binding.Count == 1) {
                if (binding.TypeIndex >= artifact.Types().size() || !push.empty()) return nullptr;
                program->PushBytes = artifact.Types()[binding.TypeIndex].Size;
                if (!program->PushBytes || program->PushBytes > device.GetCapabilities().Limits.MaxPushConstantBytes) return nullptr;
                push.push_back({0, program->PushBytes, ToShaderStages(binding.StageMask), radray::gpu::PushConstantDescriptor::D3D12Location{0, 2}, {}});
                continue;
            }
            const bool cb = kind == radray::gpu::BindingKind::CBuffer;
            const bool texture = kind == radray::gpu::BindingKind::Texture;
            const bool texel = kind == radray::gpu::BindingKind::TypedBuffer;
            if (binding.Group != (cb ? 0u : 1u) || binding.Count != 1 ||
                (!cb && !texture && !texel && kind != radray::gpu::BindingKind::Sampler)) {
                RADRAY_ERR_LOG("Unsupported vanilla shader binding: {}", name);
                return nullptr;
            }
            const uint32_t logical = cb || vk ? binding.Binding : binding.Binding * 2 + (texture || texel ? 0u : 1u);
            bindings[binding.Group].push_back({
                logical,
                *kind,
                1,
                ToShaderStages(binding.StageMask),
                radray::gpu::D3D12BindingDescriptor{
                    cb ? radray::gpu::BindingRegisterClass::CBV : texture || texel ? radray::gpu::BindingRegisterClass::SRV
                                                                                   : radray::gpu::BindingRegisterClass::Sampler,
                    cb ? logical : logical / 2,
                    binding.Group,
                },
                radray::gpu::VulkanBindingDescriptor{binding.Group, logical},
            });
            if (cb || texture || texel) {
                program->Required[binding.Group].push_back(logical);
                if (logical < 64u)
                    program->RequiredMask[binding.Group] |= uint64_t{1} << logical;
                else
                    program->HasHighBindings = true;
            } else {
                program->Samplers.push_back(logical);
                if (logical < 64u)
                    program->SamplerMask |= uint64_t{1} << logical;
                else
                    program->HasHighBindings = true;
            }
        }
        radray::vector<radray::gpu::PipelineLayoutGroupDescriptor> groups;
        for (uint32_t group = 0; group < 2; ++group) {
            if (!bindings[group].empty()) {
                program->Groups[group] = device.CreateBindingGroupLayout({bindings[group]}).Release();
                if (!program->Groups[group]) return nullptr;
                groups.push_back({group, program->Groups[group].get()});
            }
        }
        if (program->DrawBasesLayout) groups.push_back({2, program->DrawBasesLayout.get()});
        program->Layout = device.CreatePipelineLayout({groups, push}).Release();
        const auto blob = vk ? radray::gpu::ShaderBlobCategory::SPIRV : radray::gpu::ShaderBlobCategory::DXIL;
        program->Vs = device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex), blob, radray::gpu::ShaderStage::Vertex}).Release();
        program->Ps = device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel), blob, radray::gpu::ShaderStage::Pixel}).Release();
        if (!program->Layout || !program->Vs || !program->Ps) return nullptr;
        cached = std::move(program);
    }
    return cached.get();
}
}  // namespace raynium
