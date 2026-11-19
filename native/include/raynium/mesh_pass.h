#pragma once
#include <radray/types.h>
#include <raynium/render_resources.h>
#include <raynium/texture_registry.h>
#include <raynium/shader_pack.h>
#include <span>
#include <utility>

namespace raynium {
enum TerrainPass : uint32_t { Solid=0, Cutout=1, Translucent=2, OitBounds=3, OitTransmittance=4, OitAccumulate=5, OitCull=6, OitComposite=7 };
struct MeshMaterialBinding {
    uint64_t sortKey{};
    radray::array<radray::IntrusivePtr<radray::GpuTexture>,2> textures;
    radray::unique_ptr<radray::gpu::ShaderParameterSet> parameters;
};

class MeshPassResources final {
public:
    static bool Create(radray::gpu::Device&,std::span<const radray::byte>,radray::unique_ptr<MeshPassResources>&);
    const radray::shared_ptr<radray::gpu::BindingGroupLayout>& PassGroupLayout() const { return _objects->passGroupLayout; }
    radray::gpu::GraphicsPipelineState* Pipeline() const { return _objects->programs[Solid].pipeline.get(); }
    bool SupportsWireframe() const { return bool(_objects->wireframe[0]) && bool(_objects->wireframe[1]); }
    radray::gpu::GraphicsPipelineState* Pipeline(uint32_t pass,radray::gpu::TextureFormat=radray::gpu::TextureFormat::RGBA8_UNORM,bool flipY=false,bool wireframe=false) const {
        if(wireframe && pass<=Translucent) return _objects->wireframe[flipY?1:0].get();
        const auto& p=_objects->programs[pass]; return flipY && pass<OitCull?p.flippedPipeline.get():p.pipeline.get();
    }
    bool PrepareOutput(radray::gpu::Device&,radray::gpu::TextureFormat);
    const radray::shared_ptr<radray::gpu::BindingGroupLayout>& OitGroupLayout() const { return _objects->oitGroupLayout; }
    bool CreateMaterialBinding(radray::gpu::Device&,const rn_render_frame_desc&,const TextureRegistry&,
        MeshMaterialBinding&) const;
private:
    struct Program {
        radray::unique_ptr<ShaderProgram> program;
        radray::unique_ptr<radray::gpu::PipelineLayout> pipelineLayout;
        radray::unique_ptr<radray::gpu::Shader> vs,ps;

        radray::unique_ptr<radray::gpu::GraphicsPipelineState> pipeline,flippedPipeline;
    };
    struct Objects {
        radray::shared_ptr<radray::gpu::BindingGroupLayout> passGroupLayout,materialGroupLayout,oitGroupLayout;
        radray::array<Program,8> programs;
        radray::array<radray::unique_ptr<radray::gpu::GraphicsPipelineState>,2> wireframe;
    };
    explicit MeshPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
