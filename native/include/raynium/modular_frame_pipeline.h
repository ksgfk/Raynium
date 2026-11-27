#pragma once

#include <cstdint>
#include <mutex>
#include <span>

#include <radray/runtime/gpu_device.h>
#include <radray/runtime/readback_queue.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/types.h>
#include <raynium/command_tape.h>
#include <raynium/output_pass.h>
#include <raynium/render_program.h>
#include <raynium/terrain_residency.h>

namespace raynium {
class ModularFramePipeline final : public radray::RenderGraphFramePipeline {
public:
    static bool Create(
        radray::DeviceContext& device,
        std::span<const radray::byte> shaders,
        bool indirect,
        radray::unique_ptr<ModularFramePipeline>& out);
    bool UsesIndirect() const noexcept;
    MeshPassResources& Resources() noexcept;
    RenderProgramLibrary& Programs() noexcept;
    void SetTape(const TerrainFrame& input, FrameTapePlan& tape, bool hasCaptures) noexcept;
    void ClearInput() noexcept;
    uint64_t SubmittedSequence() const noexcept;
    bool CompileTape(
        radray::FrameSlot& slot,
        TapeCompiler& compiler,
        const rn_batch_desc& batch,
        rn_renderer* owner,
        const StaticGeometryRegistry& meshes,
        const TerrainResidency& terrain);
    void Finish(radray::gpu::Device& device);
    radray::gpu::FenceStatus WaitFence(uint64_t sequence, uint64_t timeoutNs) noexcept;
    uint64_t SampleFence() const noexcept;
    bool HasInFlightReadbacks(uint64_t sequence) const noexcept;
    bool RegisterCaptures(std::span<const uint64_t> ids, uint64_t sequence);
    bool ReserveCapture(uint64_t id, uint32_t width, uint32_t height);
    void CancelUnsubmittedCaptures(std::span<const uint64_t> ids) noexcept;
    std::unique_lock<std::mutex> LockReadbacks();
    bool Collect(uint64_t sequence, radray::vector<HostBufferWrite>& writes, radray::vector<PublishedTimestamp>& timestamps);
    bool PollCapture(uint64_t id, uint64_t completed, bool open, rn_capture_info& info, void* dst, uint64_t capacity);
    void MeasureBatch(radray::FrameSlot& slot, bool enabled);
    void RecordTelemetry(rn_batch_telemetry report);
    bool PopTelemetry(rn_batch_telemetry& report);
    void Wait(radray::FrameSlot& slot) noexcept override;

private:
    // GPU objects reused by the slot's frames; written only after that slot's previous GPU work completed.
    struct SlotResources {
        FrameTimestamps Timestamps;
        FrameReadbacks Readbacks;
    };

    struct Measurement {
        rn_batch_telemetry Report{};
        bool Retired{};
    };

    ModularFramePipeline(
        radray::DeviceContext& device,
        radray::unique_ptr<MeshPassResources> resources,
        radray::unique_ptr<OutputPassResources> output,
        radray::unique_ptr<RenderProgramLibrary> programs,
        radray::unique_ptr<radray::gpu::Fence> completion,
        bool indirect);

    void OnFrameSlotRetired(radray::FrameSlot& slot, const radray::RDGGpuTimingSnapshot& snapshot) noexcept override;
    void ExtractFrameData(radray::FrameSlot& slot) noexcept override;
    void SetupGraph(radray::RDGBuilder& graph, radray::FrameSlot& slot) noexcept override;
    bool SubmitFrame(radray::FrameSlot& slot, radray::RDGSubmitPlan& plan) noexcept override;

    radray::unique_ptr<MeshPassResources> _resources;
    radray::unique_ptr<OutputPassResources> _output;
    radray::unique_ptr<RenderProgramLibrary> _programs;
    radray::array<SlotResources, 2> _slots;
    radray::unique_ptr<radray::gpu::Fence> _completion;
    std::mutex _readbackMutex;
    radray::AsyncReadbackQueue _readback;
    radray::unordered_map<uint64_t, uint64_t> _unreservedCaptures;
    bool _indirect{};
    bool _hasCaptures{};
    const TerrainFrame* _pending{};
    FrameTapePlan* _tape{};
    radray::deque<Measurement> _inFlightTelemetry;
    radray::deque<rn_batch_telemetry> _readyTelemetry;
    radray::array<uint64_t, 2> _inFlight{};
    uint64_t _submitted{};
};
}  // namespace raynium
