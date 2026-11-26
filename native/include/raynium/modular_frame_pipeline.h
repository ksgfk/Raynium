#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/terrain_residency.h>
#include <raynium/command_tape.h>
#include <raynium/render_program.h>
#include <raynium/output_pass.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/readback_queue.h>
#include <mutex>
#include <span>
#include <utility>

namespace raynium {
class ModularFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,bool indirect,radray::unique_ptr<ModularFramePipeline>&);
    bool UsesIndirect() const { return _indirect; }
    MeshPassResources& Resources() { return *_resources; }
    RenderProgramLibrary& Programs() { return *_programs; }
    void SetTape(const TerrainFrame& input,FrameTapePlan& tape,bool hasCaptures) noexcept { _pending=&input; _tape=&tape; _hasCaptures=hasCaptures; }
    void ClearInput() noexcept { _pending=nullptr; _tape=nullptr; _hasCaptures=false; }
    uint64_t SubmittedSequence() const noexcept { return _submitted; }
    bool CompileTape(radray::FrameSlot& slot,TapeCompiler& compiler,const rn_batch_desc& batch,rn_renderer* owner,
        const StaticGeometryRegistry& meshes,const TerrainResidency& terrain) {
        auto& resources=_slots[slot.GetIndex()];
        return compiler.Compile(batch,owner,meshes,terrain,resources.readbacks,
            [this](uint64_t id,uint32_t width,uint32_t height) { return ReserveCapture(id,width,height); },
            resources.timestamps);
    }
    void Finish(radray::gpu::Device& device);
    radray::gpu::FenceStatus WaitFence(uint64_t sequence,uint64_t timeoutNs) noexcept { return _completion->WaitForStatus(sequence,timeoutNs); }
    uint64_t SampleFence() const noexcept { return _completion->GetCompletedValue(); }
    bool HasInFlightReadbacks(uint64_t sequence) const noexcept;
    bool RegisterCaptures(std::span<const uint64_t> ids,uint64_t sequence);
    bool ReserveCapture(uint64_t id,uint32_t width,uint32_t height);
    void CancelUnsubmittedCaptures(std::span<const uint64_t> ids) noexcept;
    std::unique_lock<std::mutex> LockReadbacks() { return std::unique_lock<std::mutex>(_readbackMutex); }
    bool Collect(uint64_t sequence,radray::vector<HostBufferWrite>& writes,radray::vector<PublishedTimestamp>& timestamps);
    bool PollCapture(uint64_t id,uint64_t completed,bool open,rn_capture_info& info,void* dst,uint64_t capacity);
    void MeasureBatch(radray::FrameSlot& slot, bool enabled);
    void RecordTelemetry(rn_batch_telemetry report);
    bool PopTelemetry(rn_batch_telemetry&);
    void Wait(radray::FrameSlot&) noexcept override;
private:
    void OnFrameSlotRetired(radray::FrameSlot&,const radray::RDGGpuTimingSnapshot&) noexcept override;
    void ExtractFrameData(radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,radray::FrameSlot&) noexcept override;
    bool SubmitFrame(radray::FrameSlot&,radray::RDGSubmitPlan&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        FrameTimestamps timestamps;
        FrameReadbacks readbacks;
    };
    ModularFramePipeline(radray::DeviceContext& device,radray::unique_ptr<MeshPassResources> resources,
        radray::unique_ptr<OutputPassResources> output,radray::unique_ptr<RenderProgramLibrary> programs,
        radray::unique_ptr<radray::gpu::Fence> completion,bool indirect)
        :RenderGraphFramePipeline(device,2,8u<<20),_resources(std::move(resources)),_output(std::move(output)),
         _programs(std::move(programs)),_completion(std::move(completion)),_readback(device.GetDevice(),4096),_indirect(indirect) {}
    radray::unique_ptr<MeshPassResources> _resources;
    radray::unique_ptr<OutputPassResources> _output;
    radray::unique_ptr<RenderProgramLibrary> _programs;
    radray::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::gpu::Fence> _completion;
    std::mutex _readbackMutex;
    radray::AsyncReadbackQueue _readback;
    radray::unordered_map<uint64_t,uint64_t> _unreservedCaptures;
    bool _indirect;
    bool _hasCaptures{};
    const TerrainFrame* _pending{};
    FrameTapePlan* _tape{};
    struct Measurement { rn_batch_telemetry report; bool retired{}; };
    radray::deque<Measurement> _inFlightTelemetry;
    radray::deque<rn_batch_telemetry> _readyTelemetry;
    radray::array<uint64_t,2> _inFlight{};
    uint64_t _submitted{};
};
}
