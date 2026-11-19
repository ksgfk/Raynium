#pragma once
#include <radray/types.h>
#include <raynium/texture_registry.h>
#include <raynium/shader_pack.h>
#include <raynium/frame_upload.h>
#include <span>
#include <utility>

namespace raynium {
struct GuiDraw {
    rn_gui_draw desc{};
    radray::Rect scissor{};
    radray::IntrusivePtr<radray::GpuTexture> texture;
    radray::gpu::TextureView* view{};
    radray::gpu::Sampler* sampler{};
    radray::gpu::ShaderParameterSet* parameters{};
};
struct GuiFrameSlot {
    rn_frame_metrics* metrics{};
    radray::array<radray::gpu::ShaderParameterSet*,2> passParameters{};
    radray::vector<GuiDraw> draws;
    std::span<const radray::byte> vertices;
    float width{},height{},yDirection{};
    uint32_t vertexCount{};
    radray::RDGBufferHandle vertexBuffer,indexBuffer;
    radray::array<radray::RDGBufferHandle,2> constants;
};
class GuiPassResources final {
public:
    static bool Create(radray::gpu::Device&,std::span<const radray::byte>,radray::unique_ptr<GuiPassResources>&);
    bool PrepareOutput(radray::gpu::Device&,radray::gpu::TextureFormat);
    bool Prepare(radray::gpu::Device&,GuiFrameSlot&,const TextureRegistry&,const rn_render_frame_desc&,uint32_t width,uint32_t height);
    void Upload(radray::RDGBuilder&,GuiFrameSlot&);
    void AddPass(radray::RDGBuilder&,radray::FrameSlot&,GuiFrameSlot&,radray::RDGTextureHandle,uint32_t,uint32_t,FrameTextureImports&,uint32_t first=0,uint32_t count=UINT32_MAX);
private:
    struct Program {
        radray::unique_ptr<ShaderProgram> program;
        radray::unique_ptr<radray::gpu::Shader> vs,ps;
        radray::unique_ptr<radray::gpu::PipelineLayout> layout;
    };
    struct Objects {
        radray::shared_ptr<radray::gpu::BindingGroupLayout> passLayout,textureLayout;
        radray::array<Program,3> programs;
        radray::array<radray::unique_ptr<radray::gpu::GraphicsPipelineState>,12> pipelines;
    };
    explicit GuiPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
