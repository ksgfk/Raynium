#pragma once
#include <raynium/render_resources.h>
#include <radray/runtime/renderer.h>
#include <radray/runtime/pipeline_cache.h>

namespace raynium {
struct MeshProgram {
    radray::unique_ptr<radray::render::Shader> vs,ps;
    radray::unique_ptr<radray::render::BindingGroupLayout> group;
    radray::unique_ptr<radray::render::PipelineLayout> layout;
    std::unique_ptr<radray::GraphicsPipelineCache> cache;
    radray::render::GraphicsPipelineState* pipeline{};
    bool vertexInput{},textures{};
};
struct MeshMaterialBinding {
    uint64_t version{},textureVersion{};
    radray::unique_ptr<radray::render::Sampler> albedoSampler,lightSampler;
    radray::unique_ptr<radray::render::ShaderParameterSet> parameters;
};
// Built-in pass state and resolution of logical bindings to a safe frame slot.
class MeshPassResources final {
public:
    static bool Create(radray::render::Device&,radray::unique_ptr<MeshPassResources>&,radray::string&);
    const MeshProgram& Program(uint32_t id) const { return _programs.at(id-1); }
    bool PrepareBinding(radray::render::Device&,radray::render::Buffer& constants,
        std::map<uint64_t,MeshMaterialBinding>&,const radray::MeshDrawCommand&,SharedTextures*,radray::string&);
private:
    explicit MeshPassResources(std::array<MeshProgram,2>&& programs):_programs(std::move(programs)) {}
    static bool CreateProgram(radray::render::Device&,uint32_t,MeshProgram&,radray::string&);
    std::array<MeshProgram,2> _programs;
};
}
