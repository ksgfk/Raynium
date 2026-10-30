#pragma once
#include <raynium/terrain_scene.h>
#include <radray/runtime/mesh_pass_processor.h>

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
class SolidFramePipeline final : public radray::FramePipeline {
public:
    static bool Create(RayniumRuntime&,radray::unique_ptr<SolidFramePipeline>&,radray::string&);
    const MeshPassResources& Resources() const { return *_resources; }
    void BeginFrame(TerrainFrame& frame,const MaterialBindingMap* bindings) noexcept {
        _pending=&frame; _processor.BeginSync(bindings);
    }
    void EndFrame() noexcept { _pending=nullptr; _processor.EndSync(); }
    bool ReadyForFrame(radray::string&);
    bool Finish(radray::string&);
    std::span<const radray::MeshPassRegistration> GetMeshPassRegistrations() noexcept override { return _registrations; }
    void Extract(const radray::Scene&,radray::FrameSlot&) noexcept override;
    void Record(radray::FrameSlot&) noexcept override;
    void Wait(radray::FrameSlot&) noexcept override;
private:
    struct SlotResources {
        RenderBuffer passConstants;
        radray::unique_ptr<radray::render::ShaderParameterSet> passParameterSet;
    };
    struct FrameData {
        explicit FrameData(TerrainFrame&& input) noexcept :input(std::move(input)) {}
        TerrainFrame input;
        uint64_t completion{};
    };
    SolidFramePipeline(radray::unique_ptr<MeshPassResources> resources,std::array<SlotResources,2>&& slots,
        radray::unique_ptr<radray::render::Fence> completion)
        :_resources(std::move(resources)),_slots(std::move(slots)),_completion(std::move(completion)),
         _processor(_defaultContext,*_resources),_registrations{{{{0},&_processor}}} {}
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
