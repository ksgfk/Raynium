#include <raynium/render_program.h>
#include <radray/logger.h>
#include <algorithm>

namespace raynium {
using namespace radray::gpu;
namespace {
ShaderStages Stages(uint32_t mask) {
    ShaderStages stages{};
    if(mask&(1u<<uint8_t(radray::shader::ShaderStage::Vertex))) stages|=ShaderStage::Vertex;
    if(mask&(1u<<uint8_t(radray::shader::ShaderStage::Pixel))) stages|=ShaderStage::Pixel;
    return stages;
}
}
bool RenderProgramLibrary::Create(std::span<const radray::byte> bytes,radray::unique_ptr<RenderProgramLibrary>& result) {
    if(result) return false;
    auto owner=radray::unique_ptr<RenderProgramLibrary>(new RenderProgramLibrary({bytes.begin(),bytes.end()}));
    if(!ReadShaderPack(owner->_bytes,owner->_entries)) return false;
    result=std::move(owner); return true;
}
RasterProgram* RenderProgramLibrary::GetProgram(Device& device,std::string_view name,std::span<const radray::byte> external) {
    const bool vk=device.GetBackend()==GpuBackend::Vulkan;
    auto& cached=_programs[radray::string(name)];
    if(!cached) {
        auto program=radray::make_unique<RasterProgram>();
        const auto lane=vk?radray::shader::ShaderTarget::SPIRV:radray::shader::ShaderTarget::DXIL;
        if(external.empty()) {
            if(!ShaderProgram::Create(_entries,name,lane,program->program)) return nullptr;
        } else {
            program->externalBytes.assign(external.begin(),external.end());
            radray::vector<ShaderPackEntry> entries;
            if(!ReadShaderPack(program->externalBytes,entries) || entries.size()!=2 ||
               entries[0].name!=name || entries[1].name!=name || !ShaderProgram::Create(entries,name,lane,program->program)) return nullptr;
        }
        const auto& artifact=program->program->Artifact();
        if(artifact.RootConstants().size()>1 || !artifact.Samplers().empty()) return nullptr;
        radray::vector<PushConstantDescriptor> push;
        for(const auto& root:artifact.RootConstants()) {
            if(!vk || root.Offset || !root.Size || root.Size>device.GetCapabilities().Limits.MaxPushConstantBytes) return nullptr;
            program->pushBytes=root.Size;
            push.push_back({0,root.Size,Stages(root.StageMask),{},PushConstantDescriptor::VulkanLocation{0}});
        }
        radray::array<radray::vector<BindingDescriptor>,2> bindings;
        for(const auto& binding:artifact.Bindings()) {
            const auto kind=radray::shader::DecodeBindingKind(binding.Type);
            if(!vk && binding.Group==3 && binding.Binding==0 && kind==BindingKind::CBuffer && binding.Count==1) {
                const BindingDescriptor desc{0,BindingKind::CBuffer,1,ShaderStage::Vertex,
                    D3D12BindingDescriptor{BindingRegisterClass::CBV,0,3},{}};
                program->drawBasesLayout=device.CreateBindingGroupLayout({{&desc,1}}).Release();
                if(!program->drawBasesLayout) return nullptr;
                continue;
            }
            if(!vk && binding.Group==2 && binding.Binding==0 && kind==BindingKind::CBuffer && binding.Count==1) {
                if(binding.TypeIndex>=artifact.Types().size() || !push.empty()) return nullptr;
                program->pushBytes=artifact.Types()[binding.TypeIndex].Size;
                if(!program->pushBytes || program->pushBytes>device.GetCapabilities().Limits.MaxPushConstantBytes) return nullptr;
                push.push_back({0,program->pushBytes,Stages(binding.StageMask),PushConstantDescriptor::D3D12Location{0,2},{}});
                continue;
            }
            const bool cb=kind==BindingKind::CBuffer,texture=kind==BindingKind::Texture;
            const bool texel=kind==BindingKind::TypedBuffer;
            if(binding.Group!=(cb?0u:1u) || binding.Count!=1 || (!cb && !texture && !texel && kind!=BindingKind::Sampler)) {
                RADRAY_ERR_LOG("Unsupported vanilla shader binding: {}",name); return nullptr;
            }
            const uint32_t logical=cb || vk?binding.Binding:binding.Binding*2+(texture || texel?0u:1u);
            bindings[binding.Group].push_back({logical,*kind,1,Stages(binding.StageMask),
                D3D12BindingDescriptor{cb?BindingRegisterClass::CBV:texture || texel?BindingRegisterClass::SRV:BindingRegisterClass::Sampler,
                    cb?logical:logical/2,binding.Group},VulkanBindingDescriptor{binding.Group,logical}});
            if(cb || texture || texel) {
                program->required[binding.Group].push_back(logical);
                if(logical<64u) program->requiredMask[binding.Group]|=uint64_t{1}<<logical;
                else program->hasHighBindings=true;
            } else {
                program->samplers.push_back(logical);
                if(logical<64u) program->samplerMask|=uint64_t{1}<<logical;
                else program->hasHighBindings=true;
            }
        }
        radray::vector<PipelineLayoutGroupDescriptor> groups;
        for(uint32_t group=0;group<2;++group) if(!bindings[group].empty()) {
            program->groups[group]=device.CreateBindingGroupLayout({bindings[group]}).Release();
            if(!program->groups[group]) return nullptr;
            groups.push_back({group,program->groups[group].get()});
        }
        if(program->drawBasesLayout) groups.push_back({2,program->drawBasesLayout.get()});
        program->layout=device.CreatePipelineLayout({groups,push}).Release();
        const auto blob=vk?ShaderBlobCategory::SPIRV:ShaderBlobCategory::DXIL;
        program->vs=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex),blob,ShaderStage::Vertex}).Release();
        program->ps=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel),blob,ShaderStage::Pixel}).Release();
        if(!program->layout || !program->vs || !program->ps) return nullptr;
        cached=std::move(program);
    }
    return cached.get();
}
}
