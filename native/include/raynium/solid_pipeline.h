#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/terrain_residency.h>
#include <raynium/gui_pass.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <span>
#include <utility>

namespace raynium {
class SolidFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,bool indirect,radray::unique_ptr<SolidFramePipeline>&);
    MeshPassResources& Resources() { return *_resources; }
    void SetInput(const TerrainFrame& frame) noexcept { _pending=&frame; }
    void ClearInput() noexcept { _pending=nullptr; }
    void Finish();
    // Call after scheduler.BeginFrame(); lists stay read-only through RunFrame.
    SlotUploads GetUploads(radray::FrameSlot&) noexcept;
    bool PrepareGui(radray::FrameSlot& slot,const UploadedTextures& textures,const rn_render_frame_desc& frame,uint32_t width,uint32_t height) {
        return _gui->Prepare(slot.GetDevice(),_slots[slot.GetIndex()].gui,textures,frame,width,height);
    }
    bool PrepareOutput(radray::render::TextureFormat format,radray::render::Device& device) {
        return _resources->PrepareOutput(device,format) && _gui->PrepareOutput(device,format);
    }
    void Wait(radray::FrameSlot&) noexcept override;
private:
    void ExtractFrameData(radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,radray::FrameSlot&) noexcept override;
    void SubmitFrame(radray::FrameSlot&,radray::render::CommandBuffer&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        GuiFrameSlot gui;
        TerrainFrameLists lists;
        RenderBuffer passConstants;
        radray::unique_ptr<radray::render::ShaderParameterSet> passParameterSet;
        radray::unique_ptr<radray::BatchedBufferUpload> staging;
        radray::unique_ptr<radray::MappedUploadBuffer> instances,indirect;
        radray::unique_ptr<radray::MappedUploadBuffer> textureStaging;
        radray::vector<TextureCopy> textureCopies;
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
    SolidFramePipeline(radray::DeviceContext& device,radray::unique_ptr<MeshPassResources> resources,radray::unique_ptr<GuiPassResources> gui,
        radray::array<SlotResources,2>&& slots,radray::unique_ptr<radray::render::Fence> completion,bool indirect)
        :RenderGraphFramePipeline(device,2),_resources(std::move(resources)),_gui(std::move(gui)),_slots(std::move(slots)),_completion(std::move(completion)),
         _indirect(indirect),_nativeLimit(device.GetDevice().GetCapabilities().Limits.MaxDrawIndirectCount) {}
    radray::unique_ptr<MeshPassResources> _resources;
    radray::unique_ptr<GuiPassResources> _gui;
    radray::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::render::Fence> _completion;
    bool _indirect{};
    uint32_t _nativeLimit{};
    const TerrainFrame* _pending{};
    radray::array<FrameData*,2> _frames{}; // Borrowed only until Wait; arena owns each record.
    uint64_t _submitted{};
    radray::array<double,120> _gpuSamples{};
    size_t _gpuSampleCount{};
    radray::array<radray::array<double,120>,8> _stageSamples{};
    radray::array<size_t,8> _stageCounts{};
};
}
