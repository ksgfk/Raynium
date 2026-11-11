#pragma once
#include <raynium/terrain_residency.h>
#include <radray/runtime/render_graph_pipeline.h>

namespace raynium {
class SolidFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(RayniumRuntime&,std::span<const radray::byte>,bool indirect,radray::unique_ptr<SolidFramePipeline>&,radray::string&);
    const MeshPassResources& Resources() const { return *_resources; }
    void SetInput(const TerrainFrame& frame) noexcept { _pending=&frame; }
    void ClearInput() noexcept { _pending=nullptr; }
    bool Finish(radray::string&);
    // Call after scheduler.BeginFrame(); lists stay read-only through RunFrame.
    SlotUploads GetUploads(radray::FrameSlot&) noexcept;
    void Wait(radray::FrameSlot&) noexcept override;
private:
    void ExtractFrameData(radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,radray::FrameSlot&) noexcept override;
    void SubmitFrame(radray::FrameSlot&,radray::render::CommandBuffer&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        TerrainFrameLists lists;
        RenderBuffer passConstants;
        radray::unique_ptr<radray::render::ShaderParameterSet> passParameterSet;
        radray::unique_ptr<radray::BatchedBufferUpload> staging;
        radray::unique_ptr<radray::MappedUploadBuffer> instances,indirect;
        radray::unique_ptr<radray::render::QueryPool> timestamps; // Empty when the device has no timestamp queries.
        RenderBuffer timing;
        std::array<radray::unique_ptr<radray::render::ShaderParameterSet>,4> oitParameters;
    };
    struct FrameData {
        explicit FrameData(const TerrainFrame& input) noexcept :input(input),sequence(input.parameters.output_sequence),flags(input.parameters.flags) {}
        TerrainFrame input;
        uint64_t completion{},sequence{};
        uint32_t flags{};
        uint32_t timingMask{};
        std::array<uint32_t,6> visibleCommands{};
        uint64_t directCalls{},indirectCalls{},bindingChanges{},submittedIndices{},submittedRecords{},submittedBatches{};
    };
    SolidFramePipeline(RayniumRuntime& device,radray::unique_ptr<MeshPassResources> resources,
        std::array<SlotResources,2>&& slots,radray::unique_ptr<radray::render::Fence> completion,bool indirect)
        :RenderGraphFramePipeline(device,2),_resources(std::move(resources)),_slots(std::move(slots)),_completion(std::move(completion)),
         _indirect(indirect),_nativeLimit(device.GetDevice().GetCapabilities().Limits.MaxDrawIndirectCount) {}
    radray::unique_ptr<MeshPassResources> _resources;
    std::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::render::Fence> _completion;
    bool _indirect{};
    uint32_t _nativeLimit{};
    const TerrainFrame* _pending{};
    std::array<FrameData*,2> _frames{}; // Borrowed only until Wait; arena owns each record.
    uint64_t _submitted{};
    std::array<double,120> _gpuSamples{};
    size_t _gpuSampleCount{};
    std::array<std::array<double,120>,8> _stageSamples{};
    std::array<size_t,8> _stageCounts{};
};
}
