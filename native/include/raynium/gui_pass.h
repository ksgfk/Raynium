#pragma once
#include <radray/types.h>
#include <raynium/texture_resources.h>
#include <raynium/shader_pack.h>
#include <raynium/frame_upload.h>
#include <span>
#include <utility>

namespace raynium {
struct GuiDraw {
    rn_gui_draw desc{};
    radray::Rect scissor{};
    radray::GpuTexture* texture{}; // Borrowed from the slot cache through command storage reset.
    radray::render::ShaderParameterSet* parameters{};
};
struct GuiFrameSlot {
    struct TextureParameters {
        radray::IntrusivePtr<radray::GpuTexture> texture;
        radray::render::Sampler* sampler{};
        radray::unique_ptr<radray::render::ShaderParameterSet> parameters;
    };
    radray::array<radray::unique_ptr<radray::render::ShaderParameterSet>,2> passParameters;
    radray::array<radray::RDGBufferSlice,2> boundConstants;
    radray::vector<GuiDraw> draws;
    radray::vector<TextureParameters> textureParameters;
    std::span<const radray::byte> vertices;
    float width{},height{},yDirection{};
    uint32_t vertexCount{};
};
class GuiPassResources final {
public:
    static bool Create(radray::render::Device&,std::span<const radray::byte>,radray::unique_ptr<GuiPassResources>&);
    bool CreateSlot(radray::render::Device&,GuiFrameSlot&);
    bool PrepareOutput(radray::render::Device&,radray::render::TextureFormat);
    bool Prepare(radray::render::Device&,GuiFrameSlot&,const UploadedTextures&,const rn_render_frame_desc&,uint32_t width,uint32_t height);
    void AddPass(radray::RDGBuilder&,radray::FrameSlot&,GuiFrameSlot&,radray::RDGTextureHandle,uint32_t,uint32_t);
private:
    struct Program {
        radray::unique_ptr<ShaderProgram> program;
        radray::unique_ptr<radray::render::Shader> vs,ps;
        radray::unique_ptr<radray::render::PipelineLayout> layout;
    };
    struct Objects {
        radray::unique_ptr<radray::render::BindingGroupLayout> passLayout,textureLayout;
        radray::array<Program,3> programs;
        radray::array<radray::unique_ptr<radray::render::GraphicsPipelineState>,12> pipelines;
        radray::unordered_map<uint64_t,radray::unique_ptr<radray::render::Sampler>> samplers;
    };
    explicit GuiPassResources(radray::unique_ptr<Objects> objects):_objects(std::move(objects)) {}
    radray::unique_ptr<Objects> _objects;
};
}
