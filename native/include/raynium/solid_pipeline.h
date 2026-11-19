#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/terrain_residency.h>
#include <raynium/gui_pass.h>
#include <raynium/output_pass.h>
#include <raynium/scene_pass.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/readback_queue.h>
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
    void CancelUnsubmittedCaptures() noexcept;
    bool PollCapture(uint64_t sequence,rn_capture_info&,void*,uint64_t capacity);
    bool RequestCapture(const TextureRegistry&,const rn_capture_request&);
    // Call after scheduler.BeginFrame(); lists stay read-only through RunFrame.
    SlotUploads GetUploads(radray::FrameSlot&) noexcept;
    bool PrepareScene(radray::FrameSlot& slot,const TextureRegistry& textures,const rn_render_frame_desc& frame) {
        return _scene->Prepare(slot.GetDevice(),_slots[slot.GetIndex()].scene,textures,frame);
    }
    void ClearSceneInput(radray::FrameSlot& slot) noexcept { _slots[slot.GetIndex()].scene.ClearBorrowed(); }
    bool PrepareGui(radray::FrameSlot& slot,const TextureRegistry& textures,const rn_render_frame_desc& frame,uint32_t width,uint32_t height) {
        return _gui->Prepare(slot.GetDevice(),_slots[slot.GetIndex()].gui,textures,frame,width,height);
    }
    bool PrepareOutput(radray::gpu::TextureFormat format,radray::gpu::Device& device) {
        return format==radray::gpu::TextureFormat::BGRA8_UNORM && _resources->PrepareOutput(device,radray::gpu::TextureFormat::RGBA8_UNORM)
            && _gui->PrepareOutput(device,radray::gpu::TextureFormat::RGBA8_UNORM);
    }
    void Wait(radray::FrameSlot&) noexcept override;
private:
    void OnGpuTimingCompleted(radray::FrameSlot&,const radray::RDGGpuTimingSnapshot&) noexcept override;
    void ExtractFrameData(radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,radray::FrameSlot&) noexcept override;
    void SubmitFrame(radray::FrameSlot&,radray::gpu::CommandBuffer&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        rn_frame_metrics metrics{};
        GuiFrameSlot gui;
        SceneFrameSlot scene;
        TerrainFrameLists lists;
        radray::vector<GeometryUpload> geometry;
        radray::IntrusivePtr<radray::GpuTexture> color;
        bool profileStages{};
        radray::array<uint32_t,6> visibleCommands{};
    };
    struct FrameData {
        explicit FrameData(const TerrainFrame& input) noexcept :input(input) {}
        TerrainFrame input;
        uint64_t completion{};
        bool renderedSceneColor{};
        uint64_t directCalls{},indirectCalls{},bindingChanges{},submittedIndices{},submittedRecords{},submittedBatches{};
    };
    SolidFramePipeline(radray::DeviceContext& device,radray::unique_ptr<MeshPassResources> resources,radray::unique_ptr<GuiPassResources> gui,radray::unique_ptr<OutputPassResources> output,radray::unique_ptr<ScenePassResources> scene,
        radray::array<SlotResources,2>&& slots,radray::unique_ptr<radray::gpu::Fence> completion,bool indirect)
        :RenderGraphFramePipeline(device,2,8u<<20),_resources(std::move(resources)),_gui(std::move(gui)),_output(std::move(output)),_scene(std::move(scene)),_slots(std::move(slots)),_completion(std::move(completion)),_readback(device.GetDevice()),
         _indirect(indirect),_nativeLimit(device.GetDevice().GetCapabilities().Limits.MaxDrawIndirectCount) {}
    radray::unique_ptr<MeshPassResources> _resources;
    radray::unique_ptr<GuiPassResources> _gui;
    radray::unique_ptr<OutputPassResources> _output;
    radray::unique_ptr<ScenePassResources> _scene;
    radray::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::gpu::Fence> _completion;
    radray::AsyncReadbackQueue _readback;
    bool _indirect{};
    uint32_t _nativeLimit{};
    const TerrainFrame* _pending{};
    radray::array<FrameData*,2> _frames{}; // Borrowed only until Wait; arena owns each record.
    uint64_t _submitted{};
    uint64_t _gpuSequence{},_gpuNanoseconds{};
    struct CaptureSource { rn_capture_request request; radray::IntrusivePtr<radray::GpuTexture> texture; uint32_t width{},height{}; bool inlineRequest{}; };
    radray::vector<CaptureSource> _captureRequests;
    radray::IntrusivePtr<radray::GpuTexture> _lastColor;
    radray::array<double,120> _gpuSamples{};
    size_t _gpuSampleCount{};
    radray::array<radray::array<double,120>,8> _stageSamples{};
    radray::array<size_t,8> _stageCounts{};
};
}
