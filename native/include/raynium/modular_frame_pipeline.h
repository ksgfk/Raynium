#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/terrain_residency.h>
#include <raynium/command_tape.h>
#include <raynium/render_program.h>
#include <raynium/output_pass.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/readback_queue.h>
#include <span>
#include <utility>

namespace raynium {
class ModularFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,bool indirect,radray::unique_ptr<ModularFramePipeline>&);
    bool UsesIndirect() const { return _indirect; }
    MeshPassResources& Resources() { return *_resources; }
    RenderProgramLibrary& Programs() { return *_programs; }
    void SetTape(const TerrainFrame& input,CommandTape& tape,BufferRegistry& buffers) noexcept { _pending=&input; _tape=&tape; _buffers=&buffers; }
    void ClearInput() noexcept { _pending=nullptr; _tape=nullptr; _buffers=nullptr; }
    uint64_t CompletedSequence() const noexcept { return _completed; }
    uint64_t SubmittedSequence() const noexcept { return _submitted; }
    bool PrepareTape(radray::FrameSlot& slot,CommandTape& tape) {
        return tape.PrepareReadbacks(slot.GetDevice(),_slots[slot.GetIndex()].readbacks) && tape.ReserveReadbacks(_readback) && _slots[slot.GetIndex()].timestamps.Prepare(slot.GetDevice(),slot.GetQueue(),tape.Timestamps());
    }
    void Finish(radray::gpu::Device& device);
    void WaitForFence(uint64_t sequence,uint64_t timeoutMs) { _completion->WaitFor(sequence,timeoutMs); }
    uint64_t SampleFence() const { return _completion->GetCompletedValue(); }
    void Collect(uint64_t sequence, radray::vector<HostBufferWrite>& writes);
    bool PollCapture(uint64_t id, uint64_t completed, rn_capture_info&, void*, uint64_t capacity);
    void MeasureBatch(radray::FrameSlot& slot, bool enabled);
    void RecordTelemetry(rn_batch_telemetry report);
    void RetireSlot(uint32_t index);
    bool PopTelemetry(rn_batch_telemetry&);
    bool PrepareOutput(radray::gpu::TextureFormat format,radray::gpu::Device&) const {
        return format==radray::gpu::TextureFormat::BGRA8_UNORM;
    }
    void Wait(radray::FrameSlot&) noexcept override;
private:
    void OnGpuTimingCompleted(radray::FrameSlot&,const radray::RDGGpuTimingSnapshot&) noexcept override;
    void ExtractFrameData(radray::FrameSlot&) noexcept override;
    void SetupGraph(radray::RDGBuilder&,radray::FrameSlot&) noexcept override;
    bool SubmitFrame(radray::FrameSlot&,radray::gpu::CommandBuffer&) noexcept override;
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        FrameTimestamps timestamps;
        FrameReadbacks readbacks;
        uint64_t measuredSequence{};
    };
    ModularFramePipeline(radray::DeviceContext& device,radray::unique_ptr<MeshPassResources> resources,
        radray::unique_ptr<OutputPassResources> output,radray::unique_ptr<RenderProgramLibrary> programs,
        radray::unique_ptr<radray::gpu::Fence> completion,bool indirect)
        :RenderGraphFramePipeline(device,2,8u<<20),_resources(std::move(resources)),_output(std::move(output)),
         _programs(std::move(programs)),_completion(std::move(completion)),_readback(device.GetDevice()),_indirect(indirect) {}
    radray::unique_ptr<MeshPassResources> _resources;
    radray::unique_ptr<OutputPassResources> _output;
    radray::unique_ptr<RenderProgramLibrary> _programs;
    radray::array<SlotResources,2> _slots;
    radray::unique_ptr<radray::gpu::Fence> _completion;
    radray::AsyncReadbackQueue _readback;
    bool _indirect;
    const TerrainFrame* _pending{};
    CommandTape* _tape{};
    BufferRegistry* _buffers{};
    struct Measurement { rn_batch_telemetry report{}; bool retired{}; };
    radray::map<uint64_t, Measurement> _measurements;
    radray::array<uint64_t,2> _inFlight{};
    uint64_t _submitted{},_completed{};
};
}
