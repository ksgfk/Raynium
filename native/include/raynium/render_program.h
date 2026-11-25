#pragma once
#include <algorithm>
#include <raynium/resource_uploads.h>
#include <raynium/shader_pack.h>
#include <raynium/frame_upload.h>


namespace raynium {
struct RasterPipelineState {
    radray::vector<radray::gpu::VertexBufferLayout> vertices;
    radray::vector<radray::gpu::VertexAttribute> attributes;
    radray::gpu::PrimitiveState primitive=radray::gpu::PrimitiveState::Default();
    std::optional<radray::gpu::DepthStencilState> depth;
    radray::vector<radray::gpu::ColorTargetState> colors;
};
struct RasterProgram {
    radray::vector<radray::byte> externalBytes;
    uint32_t pushBytes{};
    radray::unique_ptr<ShaderProgram> program;
    radray::unique_ptr<radray::gpu::Shader> vs,ps;
    radray::array<radray::shared_ptr<radray::gpu::BindingGroupLayout>,2> groups;
    radray::shared_ptr<radray::gpu::BindingGroupLayout> drawBasesLayout;
    radray::unique_ptr<radray::gpu::PipelineLayout> layout;
    radray::array<radray::vector<uint32_t>,2> required;
    radray::vector<uint32_t> samplers;
    radray::array<uint64_t,2> requiredMask{};
    uint64_t samplerMask{};
    bool hasHighBindings{};

    bool RequiresBinding(uint32_t group,uint32_t slot) const noexcept {
        if(slot<64u) return (requiredMask[group]&(uint64_t{1}<<slot))!=0;
        if(!hasHighBindings) return false;
        const auto& used=required[group];
        return std::find(used.begin(),used.end(),slot)!=used.end();
    }
    bool HasSampler(uint32_t slot) const noexcept {
        if(slot<64u) return (samplerMask&(uint64_t{1}<<slot))!=0;
        if(!hasHighBindings) return false;
        return std::find(samplers.begin(),samplers.end(),slot)!=samplers.end();
    }
};
class RenderProgramLibrary {
public:
    static bool Create(std::span<const radray::byte>,radray::unique_ptr<RenderProgramLibrary>&);
    RasterProgram* GetProgram(radray::gpu::Device&,std::string_view,std::span<const radray::byte> external={});
private:
    explicit RenderProgramLibrary(radray::vector<radray::byte> bytes):_bytes(std::move(bytes)) {}
    radray::vector<radray::byte> _bytes;
    radray::vector<ShaderPackEntry> _entries;
    radray::unordered_map<radray::string,radray::unique_ptr<RasterProgram>> _programs;
};
}
