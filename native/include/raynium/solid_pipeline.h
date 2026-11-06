#pragma once
#include <raynium/terrain_scene.h>
#include <radray/runtime/mesh_pass_processor.h>
#include <radray/runtime/render_graph_pipeline.h>

namespace raynium {
class SolidPassProcessor final : public radray::MeshPassProcessor {
public:
    SolidPassProcessor(radray::MeshPassDrawListContext& context,const MeshPassResources& resources)
        :MeshPassProcessor(context),_resources(resources) {}
    void BeginSync(const MaterialBindingMap* bindings) noexcept { _activeMaterialBindings=bindings; _hasErrors=false; }
    void EndSync() noexcept { _activeMaterialBindings=nullptr; }
    bool HasErrors() const noexcept { return _hasErrors; }
    void AddMeshBatch(const radray::MeshBatchContext&) override;
private:
    const MeshPassResources& _resources;
    const MaterialBindingMap* _activeMaterialBindings{};
    bool _hasErrors{};
};
class SolidFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(RayniumRuntime&,radray::unique_ptr<SolidFramePipeline>&,radray::string&);
    const MeshPassResources& Resources() const { return *_resources; }
    void BeginFrame(TerrainFrame& frame,const MaterialBindingMap* bindings) noexcept {
        _pending=&frame; _processor.BeginSync(bindings);
    }
    void EndFrame() noexcept { _pending=nullptr; _processor.EndSync(); }
    // Waits for the next slot before its mapped uploads may be overwritten.
    bool ReadyForFrame(radray::string&);
    bool Finish(radray::string&);
    // Preparation storage of the next slot; lists stay read-only through RunFrame, and Wait never reads them.
    SlotUploads NextUploads() noexcept;
    std::span<const radray::MeshPassRegistration> GetMeshPassRegistrations() noexcept override { return _registrations; }
    void Wait(radray::FrameSlot&) noexcept override;
private:
    void ExtractFrameData(const radray::Scene&,radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,const radray::Scene&,radray::FrameSlot&) noexcept override;
    void ExtractMeshPass(radray::MeshPassId,const radray::Scene&,radray::FrameSlot&,radray::MeshPassFrameDrawList&) noexcept override;
    void SubmitFrame(radray::FrameSlot&,radray::render::CommandBuffer&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        TerrainFrameLists lists;
        RenderBuffer passConstants;
        radray::unique_ptr<radray::render::ShaderParameterSet> passParameterSet;
        radray::unique_ptr<MappedUploadBuffer> staging,instances;
        radray::unique_ptr<radray::render::QueryPool> timestamps; // Empty when the device has no timestamp queries.
        RenderBuffer timing;
    };
    struct FrameData {
        explicit FrameData(TerrainFrame&& input) noexcept :input(std::move(input)) {}
        TerrainFrame input;
        uint64_t completion{};
    };
    SolidFramePipeline(RayniumRuntime& device,radray::unique_ptr<MeshPassResources> resources,
        std::array<SlotResources,2>&& slots,radray::unique_ptr<radray::render::Fence> completion)
        :RenderGraphFramePipeline(device,2),_resources(std::move(resources)),_slots(std::move(slots)),_completion(std::move(completion)),
         _processor(_defaultContext,*_resources),_registrations{{{{0},&_processor,radray::MeshPassSortMode::StateFirst}}} {}
    radray::unique_ptr<MeshPassResources> _resources;
    std::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::render::Fence> _completion;
    radray::DynamicMeshPassDrawListContext _defaultContext;
    SolidPassProcessor _processor;
    std::array<radray::MeshPassRegistration,1> _registrations;
    TerrainFrame* _pending{};
    std::array<FrameData*,2> _frames{}; // Borrowed only until Wait; arena owns each record.
    std::array<uint64_t,2> _slotCompletion{};
    uint64_t _submitted{};
    std::array<double,120> _gpuSamples{};
    size_t _gpuSampleCount{};
};
}
