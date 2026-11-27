#include <raynium/modular_frame_pipeline.h>

#include <algorithm>
#include <span>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/frame_upload.h>

namespace raynium {
ModularFramePipeline::ModularFramePipeline(
    radray::DeviceContext& device,
    radray::unique_ptr<MeshPassResources> resources,
    radray::unique_ptr<OutputPassResources> output,
    radray::unique_ptr<RenderProgramLibrary> programs,
    radray::unique_ptr<radray::gpu::Fence> completion,
    bool indirect)
    : RenderGraphFramePipeline(device, 2, 8u << 20),
      _resources(std::move(resources)),
      _output(std::move(output)),
      _programs(std::move(programs)),
      _completion(std::move(completion)),
      _readback(device.GetDevice(), 4096),
      _indirect(indirect) {}

bool ModularFramePipeline::UsesIndirect() const noexcept {
    return _indirect;
}

MeshPassResources& ModularFramePipeline::Resources() noexcept {
    return *_resources;
}

RenderProgramLibrary& ModularFramePipeline::Programs() noexcept {
    return *_programs;
}

void ModularFramePipeline::SetTape(const TerrainFrame& input, FrameTapePlan& tape, bool hasCaptures) noexcept {
    _pending = &input;
    _tape = &tape;
    _hasCaptures = hasCaptures;
}

void ModularFramePipeline::ClearInput() noexcept {
    _pending = nullptr;
    _tape = nullptr;
    _hasCaptures = false;
}

uint64_t ModularFramePipeline::SubmittedSequence() const noexcept {
    return _submitted;
}

bool ModularFramePipeline::CompileTape(
    radray::FrameSlot& slot,
    TapeCompiler& compiler,
    const rn_batch_desc& batch,
    rn_renderer* owner,
    const StaticGeometryRegistry& meshes,
    const TerrainResidency& terrain) {
    auto& resources = _slots[slot.GetIndex()];
    return compiler.Compile(
        batch,
        owner,
        meshes,
        terrain,
        resources.Readbacks,
        [this](uint64_t id, uint32_t width, uint32_t height) { return ReserveCapture(id, width, height); },
        resources.Timestamps);
}

radray::gpu::FenceStatus ModularFramePipeline::WaitFence(uint64_t sequence, uint64_t timeoutNs) noexcept {
    return _completion->WaitForStatus(sequence, timeoutNs);
}

uint64_t ModularFramePipeline::SampleFence() const noexcept {
    return _completion->GetCompletedValue();
}

std::unique_lock<std::mutex> ModularFramePipeline::LockReadbacks() {
    return std::unique_lock<std::mutex>(_readbackMutex);
}

bool ModularFramePipeline::Create(
    radray::DeviceContext& device,
    std::span<const radray::byte> shaders,
    bool indirect,
    radray::unique_ptr<ModularFramePipeline>& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Pipeline.Create");
    if (out) RADRAY_ABORT("Frame pipeline output occupied");
    auto& d = device.GetDevice();
    auto cache = device.GetPipelineCache();
    radray::unique_ptr<MeshPassResources> resources;
    if (!MeshPassResources::Create(d, shaders, resources, cache)) return false;
    radray::unique_ptr<OutputPassResources> output;
    if (!OutputPassResources::Create(d, shaders, output, cache)) return false;
    radray::unique_ptr<RenderProgramLibrary> scene;
    if (!RenderProgramLibrary::Create(shaders, scene)) return false;
    auto completion = d.CreateFence().Release();
    if (!completion) {
        RADRAY_ERR_LOG("Cannot create terrain completion fence");
        return false;
    }
    out = radray::unique_ptr<ModularFramePipeline>(new ModularFramePipeline(
        device, std::move(resources), std::move(output), std::move(scene), std::move(completion), indirect));
    return true;
}

bool ModularFramePipeline::HasInFlightReadbacks(uint64_t sequence) const noexcept {
    return std::ranges::any_of(_slots, [sequence](const auto& slot) { return slot.Readbacks.Sequence() == sequence; });
}

bool ModularFramePipeline::RegisterCaptures(std::span<const uint64_t> ids, uint64_t sequence) {
    if (ids.empty()) return true;
    std::lock_guard lock(_readbackMutex);
    if (ids.size() > 4096 || _readback.GetActiveRequestCount() + _unreservedCaptures.size() > 4096 - ids.size()) return false;
    size_t inserted = 0;
    for (auto id : ids) {
        if (!id || _readback.HasRequest(id) || !_unreservedCaptures.emplace(id, sequence).second) {
            for (size_t i = 0; i < inserted; ++i) _unreservedCaptures.erase(ids[i]);
            return false;
        }
        ++inserted;
    }
    return true;
}

bool ModularFramePipeline::ReserveCapture(uint64_t id, uint32_t width, uint32_t height) {
    std::lock_guard lock(_readbackMutex);
    auto it = _unreservedCaptures.find(id);
    if (it == _unreservedCaptures.end()) return false;
    if (!_readback.ReserveTexture(id, {width, height, radray::gpu::TextureFormat::RGBA8_UNORM, 1})) return false;
    _unreservedCaptures.erase(it);
    return true;
}

void ModularFramePipeline::CancelUnsubmittedCaptures(std::span<const uint64_t> ids) noexcept {
    std::lock_guard lock(_readbackMutex);
    for (auto id : ids) _unreservedCaptures.erase(id);
    _readback.CancelUnsubmitted();
}

bool ModularFramePipeline::PollCapture(uint64_t id, uint64_t completed, bool open, rn_capture_info& info, void* dst, uint64_t capacity) {
    std::lock_guard lock(_readbackMutex);
    if (_unreservedCaptures.contains(id)) {
        info = {sizeof(info)};
        return open;
    }
    radray::ReadbackResultInfo result{};
    const auto status = _readback.Poll(id, completed, result, static_cast<radray::byte*>(dst), capacity);
    if (status == radray::ReadbackStatus::Ready) {
        info = {sizeof(info), 1, result.Texture.Width, result.Texture.Height, result.PackedByteSize, uint32_t(result.PackedRowPitch), 0};
        return true;
    }
    if (status == radray::ReadbackStatus::Pending) {
        info = {sizeof(info)};
        return open;
    }
    return false;
}

bool ModularFramePipeline::Collect(
    uint64_t sequence,
    radray::vector<HostBufferWrite>& writes,
    radray::vector<PublishedTimestamp>& timestamps) {
    for (auto& slot : _slots) {
        if (!slot.Timestamps.Collect(sequence, timestamps) || !slot.Readbacks.Collect(sequence, writes)) return false;
    }
    return true;
}

void ModularFramePipeline::Finish(radray::gpu::Device& device) {
    while (_submitted && !device.IsDeviceLost() && WaitFence(_submitted, 1'000'000'000ull) == radray::gpu::FenceStatus::Timeout) {
    }
}

void ModularFramePipeline::ExtractFrameData(radray::FrameSlot&) noexcept {}

void ModularFramePipeline::SetupGraph(radray::RDGBuilder& graph, radray::FrameSlot& slot) noexcept {
    auto& resources = _slots[slot.GetIndex()];
    resources.Timestamps.Reset(graph);
    const auto& limits = slot.GetDevice().GetCapabilities().Limits;
    FrameTextureImports imports{graph, slot.GetScratchMemoryResource(), &limits};
    _pending->Uploads->AddUploads(graph, imports, _pending->ShadowWrites);
    const auto backBuffer = _pending->SurfaceFrame ? graph.ImportBackBuffer(*_pending->SurfaceFrame) : radray::RDGTextureHandle{};
    _tape->Replay(graph, imports, _readback, *_output, resources.Timestamps, resources.Readbacks, backBuffer);
    resources.Timestamps.Resolve(graph);
}

bool ModularFramePipeline::SubmitFrame(radray::FrameSlot& slot, radray::RDGSubmitPlan& plan) noexcept {
    if (_submitted >= UINT64_MAX - 1) RADRAY_ABORT("Submission sequence exhausted");
    const auto next = _submitted + 1;
    plan.AddSignalFence(_completion.get(), next);
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.QueueSubmit");
        if (!slot.GetQueue().Submit(plan.Build())) return false;
    }
    _submitted = next;
    _inFlight[slot.GetIndex()] = next;
    if (_hasCaptures) _readback.MarkSubmitted(_submitted);
    _slots[slot.GetIndex()].Timestamps.Submitted(_submitted);
    _slots[slot.GetIndex()].Readbacks.Submitted(_submitted);
    return true;
}

void ModularFramePipeline::Wait(radray::FrameSlot& slot) noexcept {
    auto& sequence = _inFlight[slot.GetIndex()];
    if (!sequence) return;
    _completion->Wait(sequence);
    sequence = 0;
}

void ModularFramePipeline::MeasureBatch(radray::FrameSlot& slot, bool enabled) {
    SetGpuProfiling(slot, enabled ? radray::RDGGpuProfilingMode::FrameOnly : radray::RDGGpuProfilingMode::Off, _submitted + 1);
}

void ModularFramePipeline::RecordTelemetry(rn_batch_telemetry report) {
    // The worker records CPU data before BeginFrame or WaitIdle can retire this slot.
    _inFlightTelemetry.push_back({report, false});
}

void ModularFramePipeline::OnFrameSlotRetired(radray::FrameSlot&, const radray::RDGGpuTimingSnapshot& snapshot) noexcept {
    for (auto& item : _inFlightTelemetry) {
        if (item.Report.sequence != snapshot.UserSequence) continue;
        if (snapshot.Status == radray::RDGGpuTimingStatus::Valid) {
            item.Report.gpu_ns = snapshot.TotalNanoseconds;
            item.Report.flags = RN_TELEMETRY_HAS_GPU_NS;
        }
        item.Retired = true;
        break;
    }
    while (!_inFlightTelemetry.empty() && _inFlightTelemetry.front().Retired) {
        _readyTelemetry.push_back(_inFlightTelemetry.front().Report);
        _inFlightTelemetry.pop_front();
    }
}

bool ModularFramePipeline::PopTelemetry(rn_batch_telemetry& report) {
    if (_readyTelemetry.empty()) return false;
    report = _readyTelemetry.front();
    _readyTelemetry.pop_front();
    return true;
}
}  // namespace raynium
