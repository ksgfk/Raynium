#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/terrain_residency.h>
#include <raynium/gui_pass.h>
#include <raynium/output_pass.h>
#include <raynium/scene_pass.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <span>
#include <utility>

namespace raynium {
class SolidFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,bool indirect,radray::unique_ptr<SolidFramePipeline>&);
    MeshPassResources& Resources() { return *_resources; }
    ScenePassResources& Scene() { return *_scene; }
    void SetInput(const TerrainFrame& frame) noexcept { _pending=&frame; }
    void ClearInput() noexcept { _pending=nullptr; }
    void Finish();
    uint64_t GpuSequence() const noexcept { return _gpuSequence; }
    uint64_t GpuNanoseconds() const noexcept { return _gpuNanoseconds; }
    void BeginMeasurements(radray::FrameSlot& slot,bool enabled) noexcept {
        auto& resources=_slots[slot.GetIndex()]; resources.metrics={};
        resources.scene.metrics=resources.gui.metrics=enabled?&resources.metrics:nullptr;
    }
    const rn_frame_metrics& Measurements(radray::FrameSlot& slot) const noexcept { return _slots[slot.GetIndex()].metrics; }
    bool PrepareCapture(radray::FrameSlot&,const rn_render_frame_desc&,uint32_t width,uint32_t height);
    bool PollCapture(uint64_t sequence,rn_capture_info&,void*,uint64_t capacity);
    bool RequestCapture(const UploadedTextures&,const rn_capture_request&);
    // Call after scheduler.BeginFrame(); lists stay read-only through RunFrame.
    SlotUploads GetUploads(radray::FrameSlot&) noexcept;
    bool PrepareScene(radray::FrameSlot& slot,const UploadedTextures& textures,const rn_render_frame_desc& frame) {
        return _scene->Prepare(slot.GetDevice(),_slots[slot.GetIndex()].scene,textures,frame);
    }
    void ClearSceneInput(radray::FrameSlot& slot) noexcept { _slots[slot.GetIndex()].scene.ClearBorrowed(); }
    bool PrepareGui(radray::FrameSlot& slot,const UploadedTextures& textures,const rn_render_frame_desc& frame,uint32_t width,uint32_t height) {
        return _gui->Prepare(slot.GetDevice(),_slots[slot.GetIndex()].gui,textures,frame,width,height);
    }
    bool PrepareOutput(radray::render::TextureFormat format,radray::render::Device& device) {
        return format==radray::render::TextureFormat::BGRA8_UNORM && _resources->PrepareOutput(device,radray::render::TextureFormat::RGBA8_UNORM)
            && _gui->PrepareOutput(device,radray::render::TextureFormat::RGBA8_UNORM);
    }
    void Wait(radray::FrameSlot&) noexcept override;
private:
    struct Capture {
        Capture(RenderBuffer buffer,radray::IntrusivePtr<TextureResource> source,uint32_t width,uint32_t height,uint64_t pitch,rn_capture_request request)
            :buffer(std::move(buffer)),source(std::move(source)),width(width),height(height),pitch(pitch),request(request) {}
        RenderBuffer buffer;
        radray::IntrusivePtr<TextureResource> source;
        uint32_t width,height;
        uint64_t pitch,completion{};
        rn_capture_request request;
    };
    void ExtractFrameData(radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,radray::FrameSlot&) noexcept override;
    void SubmitFrame(radray::FrameSlot&,radray::render::CommandBuffer&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        rn_frame_metrics metrics{};
        GuiFrameSlot gui;
        SceneFrameSlot scene;
        TerrainFrameLists lists;
        radray::RDGBufferSlice boundConstants{};
        radray::unique_ptr<radray::render::ShaderParameterSet> passParameterSet;
        OutputBinding output;
        radray::vector<GeometryUpload> geometry;
        radray::vector<TextureResource*> textureImports;
        radray::vector<radray::shared_ptr<Capture>> captures;
        radray::IntrusivePtr<TextureResource> color;
        radray::unique_ptr<radray::render::QueryPool> timestamps; // Empty when the device has no timestamp queries.
        RenderBuffer timing;
        radray::array<radray::unique_ptr<radray::render::ShaderParameterSet>,4> oitParameters;
    };
    struct FrameData {
        explicit FrameData(const TerrainFrame& input) noexcept :input(input),sequence(input.parameters.output_sequence),flags(input.parameters.flags) {}
        TerrainFrame input;
        uint64_t completion{},sequence{};
        uint32_t flags{};
        uint32_t timingMask{};
        radray::array<uint32_t,6> visibleCommands{};
        uint64_t directCalls{},indirectCalls{},bindingChanges{},submittedIndices{},submittedRecords{},submittedBatches{};
    };
    SolidFramePipeline(radray::DeviceContext& device,radray::unique_ptr<MeshPassResources> resources,radray::unique_ptr<GuiPassResources> gui,radray::unique_ptr<OutputPassResources> output,radray::unique_ptr<ScenePassResources> scene,
        radray::array<SlotResources,2>&& slots,radray::unique_ptr<radray::render::Fence> completion,bool indirect)
        :RenderGraphFramePipeline(device,2,8u<<20),_resources(std::move(resources)),_gui(std::move(gui)),_output(std::move(output)),_scene(std::move(scene)),_slots(std::move(slots)),_completion(std::move(completion)),
         _indirect(indirect),_nativeLimit(device.GetDevice().GetCapabilities().Limits.MaxDrawIndirectCount) {}
    radray::unique_ptr<MeshPassResources> _resources;
    radray::unique_ptr<GuiPassResources> _gui;
    radray::unique_ptr<OutputPassResources> _output;
    radray::unique_ptr<ScenePassResources> _scene;
    radray::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::render::Fence> _completion;
    bool _indirect{};
    uint32_t _nativeLimit{};
    const TerrainFrame* _pending{};
    radray::array<FrameData*,2> _frames{}; // Borrowed only until Wait; arena owns each record.
    uint64_t _submitted{};
    uint64_t _gpuSequence{},_gpuNanoseconds{};
    radray::unordered_map<uint64_t,radray::shared_ptr<Capture>> _captures;
    struct CaptureSource { rn_capture_request request; radray::IntrusivePtr<TextureResource> texture; };
    radray::vector<CaptureSource> _captureRequests;
    radray::IntrusivePtr<TextureResource> _lastColor;
    radray::array<double,120> _gpuSamples{};
    size_t _gpuSampleCount{};
    radray::array<radray::array<double,120>,8> _stageSamples{};
    radray::array<size_t,8> _stageCounts{};
};
}
