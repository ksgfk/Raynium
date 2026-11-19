#pragma once
#include <raynium/texture_registry.h>
#include <raynium/shader_pack.h>
#include <raynium/frame_upload.h>
#include <raynium/post_description.h>

namespace raynium {
struct ScenePipelineDescription {
    std::string_view program;
    uint32_t topology{};
    bool screen{};
    radray::array<uint64_t,2> declaredBindings{};
    radray::vector<radray::gpu::VertexBufferLayout> vertices;
    radray::vector<radray::gpu::VertexAttribute> attributes;
    radray::gpu::PrimitiveState primitive=radray::gpu::PrimitiveState::Default();
    std::optional<radray::gpu::DepthStencilState> depth;
    radray::vector<radray::gpu::ColorTargetState> colors;
};
struct SceneProgram {
    radray::unique_ptr<ShaderProgram> program;
    radray::unique_ptr<radray::gpu::Shader> vs,ps;
    radray::array<radray::shared_ptr<radray::gpu::BindingGroupLayout>,2> groups;
    radray::unique_ptr<radray::gpu::PipelineLayout> layout;
    radray::array<radray::vector<uint32_t>,2> required;
    radray::array<uint64_t,2> activeBindings{};
    uint64_t texelBindings{};
    radray::vector<uint32_t> samplers;
};
struct ScenePipeline {
    ScenePipelineDescription description;
    SceneProgram* program{};
    radray::unique_ptr<radray::gpu::GraphicsPipelineState> pipeline;
};
struct SceneBindings {
    radray::gpu::ShaderParameterSet* constants{};
    radray::gpu::ShaderParameterSet* textures{};
};
struct SceneDraw {
    rn_scene_draw desc{};
    ScenePipeline* pipeline{};
    radray::vector<radray::IntrusivePtr<radray::GpuTexture>> textures;
    radray::vector<radray::TransientParameterBinding> textureBindings;
    struct Texel { radray::IntrusivePtr<radray::GpuBuffer> owner; radray::RDGBufferHandle handle; };
    radray::vector<Texel> texels;
    radray::vector<rn_scene_binding> bindings;
    radray::RDGBufferSlice vertices{},indices{},instances{};
    radray::array<radray::IntrusivePtr<radray::GpuBuffer>,3> owners;
    radray::array<radray::RDGBufferHandle,3> geometry;
    radray::gpu::Sampler* graphSampler{};
};
struct SceneFrameSlot {
    rn_frame_metrics* metrics{};
    struct PostPass { SceneDraw draw; const PostPassDescription* description{}; };
    struct Effect { rn_scene_effect desc{}; PostChainDescription chain{}; radray::vector<PostPass> passes; };
    radray::vector<Effect> effects;
    uint32_t effectCount{};
    struct PhaseDraw { SceneDraw* draw; radray::RDGBufferHandle generated; SceneBindings groups; };
    struct Target { rn_scene_target desc{}; radray::IntrusivePtr<radray::GpuTexture> texture; };
    radray::vector<Target> targets;
    radray::vector<Target> guiGroups;
    uint32_t guiGroupCount{};
    uint32_t targetCount{};
    radray::vector<rn_scene_blob> blobs;
    radray::vector<radray::RDGBufferHandle> buffers;
    radray::vector<SceneDraw> draws;
    uint32_t drawCount{};
    radray::RDGBufferHandle backend;
    radray::array<radray::RDGBufferHandle,3> generatedIndices;
    radray::array<uint32_t,RN_SCENE_PHASE_COUNT> phaseCounts{};
    SceneDraw depthCopy;
    SceneDraw tracy;
    radray::array<SceneDraw,2> depthIntegrate;
    bool consistentDepth{};
    void ClearBorrowed() noexcept { blobs.clear(); }
};
class ScenePassResources final {
public:
    static bool Create(std::span<const radray::byte>,radray::unique_ptr<ScenePassResources>&);
    bool CreateBuffer(radray::gpu::Device&,const rn_scene_buffer_desc&);
    bool UploadBuffer(const rn_scene_buffer_upload_desc&);
    bool DestroyBuffer(uint32_t);
    void AddBufferUploads(radray::RDGBuilder&,rn_frame_metrics*);
    bool Prepare(radray::gpu::Device&,SceneFrameSlot&,const TextureRegistry&,const rn_render_frame_desc&);
    bool PrepareTracy(radray::gpu::Device&,SceneFrameSlot&);
    radray::RDGTextureHandle AddTracy(radray::RDGBuilder&,radray::FrameSlot&,SceneFrameSlot&,radray::RDGTextureHandle,uint32_t,uint32_t);
    void Upload(radray::RDGBuilder&,radray::FrameSlot&,SceneFrameSlot&);
    void AddOffscreenPasses(radray::RDGBuilder&,radray::FrameSlot&,SceneFrameSlot&,FrameTextureImports&);
    void AddPass(radray::RDGBuilder&,radray::FrameSlot&,SceneFrameSlot&,rn_scene_phase,
        radray::RDGTextureHandle color,radray::RDGTextureHandle depth,uint32_t width,uint32_t height,FrameTextureImports&,
        radray::RDGTextureHandle second={},radray::array<radray::RDGTextureHandle,3> inputs={},bool clearDepth=false,SceneFrameSlot::Target* target=nullptr);
    void AddDepthCopy(radray::RDGBuilder&,SceneFrameSlot&,radray::RDGTextureHandle source,radray::RDGTextureHandle target,uint32_t width,uint32_t height,int integrate=-1);
    void AddPostEffects(radray::RDGBuilder&,radray::FrameSlot&,SceneFrameSlot&,uint32_t phase,
        radray::RDGTextureHandle color,radray::RDGTextureHandle depth,uint32_t width,uint32_t height,radray::RDGTextureHandle composite={});
    void AddOutline(radray::RDGBuilder&,radray::FrameSlot&,SceneFrameSlot&,radray::RDGTextureHandle color,
        radray::RDGTextureHandle depth,uint32_t width,uint32_t height,FrameTextureImports&);
private:
    bool PreparePostEffects(radray::gpu::Device&,SceneFrameSlot&,const rn_render_frame_desc&);
    explicit ScenePassResources(radray::vector<radray::byte> bytes):_bytes(std::move(bytes)) {}
    ScenePipeline* GetPipeline(radray::gpu::Device&,uint32_t,bool flipped=false,bool colorOnly=false);
    radray::vector<radray::byte> _bytes;
    radray::unordered_map<radray::string,radray::unique_ptr<SceneProgram>> _programs;
    radray::unordered_map<uint32_t,radray::unique_ptr<ScenePipeline>> _pipelines;

    struct BufferUpload { radray::IntrusivePtr<radray::GpuBuffer> target; uint64_t offset; radray::vector<radray::byte> bytes; };
    radray::unordered_map<uint32_t,radray::IntrusivePtr<radray::GpuBuffer>> _buffers;
    radray::vector<BufferUpload> _uploads;
    uint32_t _lastBufferId{};
};
}
