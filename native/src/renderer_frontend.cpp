#include <raynium/renderer_frontend.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>

namespace raynium {
RendererFrontend::RendererFrontend(rn_renderer* owner, RenderChannel& session, Renderer& renderer)
    : _owner(owner), _session(session), _renderer(renderer) {
    _session.SetIdlePoll([this] { return !_readbacks.empty() && Collect() && !_readbacks.empty(); });
}

RendererFrontend::~RendererFrontend() {
    _session.SetIdlePoll({});
}

bool RendererFrontend::Open() {
    return !_flags.load(std::memory_order_acquire);
}

void RendererFrontend::RemoveModelMesh(int32_t id) {
    _pending.RemovedModelMeshes.push_back(id);
}

void RendererFrontend::Snapshot(rn_wait_result& out) {
    out.flags = _flags.load();
    out.completed_sequence = _completed.load();
    out.submitted_sequence = _submitted.load();
    out.consumed_sequence = out.flags ? _enqueued : _consumed.load();
    out.presented_frames = _presented.load();
    out.surface_generation = _generation.load();
    out.surface_recovery_state = _recovery.load();
    out.last_present_outcome = _outcome.load();
    if (_hasWrites.load(std::memory_order_acquire)) {
        std::lock_guard lock(_publishMutex);
        while (!_writes.empty() && _writes.front().Sequence <= out.completed_sequence) {
            for (const auto& write : _writes.front().Writes) ApplyHostBufferWrite(write);
            _writes.pop_front();
        }
        _hasWrites.store(!_writes.empty());
    }
}

void RendererFrontend::Advance(uint64_t done) {
    const auto submitted = _submitted.load(std::memory_order_acquire);
    const auto next = std::min({done, submitted, _ceiling.load(std::memory_order_acquire)});
    auto old = _completed.load();
    while (old < next && !_completed.compare_exchange_weak(old, next)) {
    }
}

void RendererFrontend::SettleDeviceLoss() {
    if (_renderer.Runtime().GetDevice().IsDeviceLost() && Open()) {
        _session.Barrier([this] { return Collect(true); });
    }
}

void RendererFrontend::SampleProgress() {
    if (!Open()) return;
    const auto done = _renderer.Pipeline().SampleFence();
    if (_renderer.Runtime().GetDevice().IsDeviceLost()) {
        SettleDeviceLoss();
        return;
    }
    Advance(done);
    while (!_retired.empty() && _retired.front().Sequence <= _completed.load()) _retired.pop_front();
}

bool RendererFrontend::FailInput(rn_submit_result& out) {
    RADRAY_ERR_LOG("Invalid frame submission");
    _session.Barrier([this] { return Collect(true); });
    Snapshot(out.progress);
    return false;
}

bool RendererFrontend::Submit(const rn_batch_desc& desc, Swapchain* target, rn_submit_result& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.Freeze");
    SampleProgress();
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.QueueCapacity");
        _session.WaitUntil([&] { return _session.HasCapacity() || !Open(); });
    }
    if (!Open()) {
        Snapshot(out.progress);
        return false;
    }
    auto& envelope = _envelopes[_enqueued % 2];
    envelope.Reset();
    if (!envelope.Freeze(desc)) return FailInput(out);
    for (auto range : std::span(desc.dirty_ranges, desc.dirty_count)) {
        auto* buffer = ResolveHandle(_owner, range.buffer);
        if (!buffer || !BufferContainsRange(*buffer, range.offset, range.length) || !(buffer->Usage & RN_BUFFER_MAP_WRITE) ||
            (buffer->Usage & RN_BUFFER_COPY_DST)) {
            return FailInput(out);
        }
        if (IsHostVisibleBuffer(*buffer)) {
            if (!buffer->Gpu->FlushMappedRange({range.offset, range.length})) return FailInput(out);
        } else {
            auto* copy = static_cast<radray::byte*>(envelope.Memory().allocate(range.length, 1));
            std::memcpy(copy, buffer->ShadowHost.data() + range.offset, range.length);
            envelope.Shadows.push_back({buffer->Gpu, range.offset, {copy, size_t(range.length)}});
        }
    }
    for (auto query : std::span(desc.queries, desc.query_count)) {
        if (query.reserved || !ResolveHandle(_owner, query.pool) || query.slot >= query.pool->Storage->Slots.size()) {
            return FailInput(out);
        }
    }
    const auto seq = _enqueued + 1;
    if (desc.capture_count && !_renderer.Pipeline().RegisterCaptures({desc.captures, desc.capture_count}, seq)) {
        return FailInput(out);
    }
    for (auto query : std::span(desc.queries, desc.query_count)) {
        auto& pool = *query.pool->Storage;
        std::lock_guard lock(pool.Mutex);
        pool.Slots[query.slot] = {seq, 0, false};
    }
    std::swap(envelope.Resources, _pending);
    envelope.Frontend = this;
    envelope.Sequence = _enqueued = out.sequence = seq;
    envelope.Target = target;
#ifdef RADRAY_ENABLE_PROFILER
    if ((desc.flags & RN_BATCH_MEASURE) && seq % 128 == 0) ZoneTextF("%llu", static_cast<unsigned long long>(seq));
#endif
    _session.Publish(envelope);
    Snapshot(out.progress);
    _renderer.Buffers().Advance(seq);
    return true;
}

void RendererFrontend::Consume(BatchEnvelope& envelope) {
    if (!Open()) {
        if (envelope.Batch.capture_count) {
            _renderer.Pipeline().CancelUnsubmittedCaptures({envelope.Batch.captures, envelope.Batch.capture_count});
        }
        return;
    }
    RADRAY_PROFILE_SCOPE_N("Raynium.Worker.Frame");
    FrameExecution frame{};
    auto& batch = envelope.Batch;
    const auto seq = envelope.Sequence;
    const bool ok = _renderer.SubmitFrame(
        _owner,
        envelope,
        frame,
        [&](uint64_t submitted) {
            if (seq != submitted) RADRAY_ABORT("Render mailbox sequence mismatch");
            if (batch.query_count || _renderer.Pipeline().HasInFlightReadbacks(seq)) {
                _readbacks.push_back(seq);
                _ceiling.store(_readbacks.front() - 1);
            }
            _consumed.store(seq);
            _submitted.store(seq);
            _session.NotifyProgress();
        },
        [this] { return OnSlotAvailable(); });
    if (ok && batch.surface) Present(frame.Outcome);
    if (!ok) Collect(true);
}

bool RendererFrontend::Wait(uint64_t sequence, int64_t timeout, rn_wait_result& out) {
    const auto start = std::chrono::steady_clock::now();
    auto remaining = [&]() -> int64_t {
        if (timeout < 0) return -1;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        return std::max<int64_t>(0, timeout - elapsed);
    };
    SampleProgress();
    if (sequence > _enqueued) {
        Snapshot(out);
        return false;
    }
    if (timeout && Open() && sequence > _submitted.load()) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.WaitForSubmission");
        _session.WaitUntil([&] { return !Open() || sequence <= _submitted.load(); }, remaining());
    }
    while (timeout && Open() && sequence > _completed.load() && sequence <= _submitted.load()) {
        const auto left = remaining();
        if (!left) break;
        constexpr uint64_t kSliceNs = 2'000'000;
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.WaitForGpu");
        const auto status = _renderer.Pipeline().WaitFence(sequence, left < 0 ? kSliceNs : std::min<uint64_t>(kSliceNs, left));
        if (status == radray::gpu::FenceStatus::DeviceLost) {
            SettleDeviceLoss();
            break;
        }
        if (status == radray::gpu::FenceStatus::Completed) {
            Advance(sequence);
            _session.WaitUntil([&] { return !Open() || sequence <= _completed.load(); }, remaining());
            break;
        }
        _session.PumpSentMessages();
    }
    Snapshot(out);
    return !out.flags;
}

bool RendererFrontend::Flush(rn_wait_result& out) {
    _session.Barrier([&] {
        if (Open()) {
            _renderer.Finish();
            _renderer.Runtime().GetQueue().Wait();
            Collect(_renderer.IsTerminal());
        }
        return true;
    });
    Snapshot(out);
    return !out.flags && out.completed_sequence == _enqueued;
}

bool RendererFrontend::Collect(bool terminal) {
    if (!Open()) return false;
    auto& pipe = _renderer.Pipeline();
    auto& device = _renderer.Runtime().GetDevice();
    const auto done = pipe.SampleFence();
    while (!device.IsDeviceLost() && !_readbacks.empty() && _readbacks.front() <= done) {
        const auto seq = _readbacks.front();
        radray::vector<PublishedTimestamp> timestamps;
        radray::vector<HostBufferWrite> writes;
        if (!pipe.Collect(seq, writes, timestamps) || device.IsDeviceLost()) {
            terminal = true;
            break;
        }
        for (auto& value : timestamps) {
            std::lock_guard lock(value.Pool->Mutex);
            auto& slot = value.Pool->Slots[value.Slot];
            if (slot.Sequence == value.Sequence) slot = {value.Sequence, value.Ticks, true};
        }
        if (!writes.empty()) {
            std::lock_guard lock(_publishMutex);
            _writes.push_back({seq, std::move(writes)});
            _hasWrites.store(true);
        }
        _readbacks.pop_front();
        _ceiling.store(_readbacks.empty() ? UINT64_MAX : _readbacks.front() - 1);
    }
    const bool lost = device.IsDeviceLost();
    if (!lost) Advance(done);
    OnSlotAvailableTelemetry();
    if (terminal || lost) _flags.store(RN_SUBMIT_FLAG_TERMINAL_ERROR | (lost ? RN_SUBMIT_FLAG_DEVICE_LOST : 0));
    _session.NotifyProgress();
    return Open();
}

void RendererFrontend::OnSlotAvailableTelemetry() {
    rn_batch_telemetry report{};
    if (!_renderer.Pipeline().PopTelemetry(report)) return;
    std::lock_guard lock(_telemetryMutex);
    do {
        _telemetry.push_back(report);
    } while (_renderer.Pipeline().PopTelemetry(report));
}

bool RendererFrontend::OnSlotAvailable() {
    if (!_readbacks.empty()) return Collect();
    OnSlotAvailableTelemetry();
    return Open();
}

void RendererFrontend::DeviceFailure() {
    Collect(true);
}

bool RendererFrontend::PollCapture(uint64_t id, rn_capture_info& info, void* pixels, uint64_t capacity) {
    SampleProgress();
    return _renderer.Pipeline().PollCapture(id, _completed.load(std::memory_order_acquire), Open(), info, pixels, capacity);
}

bool RendererFrontend::CreateQueryPool(uint32_t count, rn_query_pool*& out) {
    auto& runtime = _renderer.Runtime();
    if (!Open() || !count || !runtime.GetDevice().GetCapabilities().Features.TimestampQueries ||
        !runtime.GetQueue().GetTimestampValidBits()) {
        return false;
    }
    auto pool = radray::make_unique<rn_query_pool>();
    pool->Storage = radray::IntrusivePtr<QueryPoolStorage>(new QueryPoolStorage(count));
    out = _queries.Insert(_owner, std::move(pool));
    return true;
}

bool RendererFrontend::RemoveQueryPool(rn_query_pool* pool) {
    if (!ResolveHandle(_owner, pool)) return false;
    delete _queries.Remove(pool);
    return true;
}

bool RendererFrontend::Query(rn_query_pool* pool, uint32_t first, std::span<rn_query_sample> samples, rn_wait_result& out) {
    SampleProgress();
    Snapshot(out);
    if (!ResolveHandle(_owner, pool)) return false;
    auto& storage = *pool->Storage;
    std::lock_guard lock(storage.Mutex);
    if (first > storage.Slots.size() || samples.size() > storage.Slots.size() - first) return false;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& slot = storage.Slots[first + i];
        samples[i] = {slot.Available && slot.Sequence <= out.completed_sequence ? slot.Sequence : 0, slot.Ticks};
    }
    return true;
}

bool RendererFrontend::CreateTexture(const rn_texture_desc& desc, rn_texture*& out) {
    auto& device = _renderer.Runtime().GetDevice();
    if (!Open() || !ValidateTextureDesc(device.GetCapabilities(), desc)) return false;
    auto gpu = CreateSceneTexture(device, desc);
    if (!gpu) {
        SettleDeviceLoss();
        return false;
    }
    auto texture = radray::make_unique<rn_texture>();
    texture->Desc = desc;
    texture->Texture = std::move(gpu);
    out = _textures.Insert(_owner, std::move(texture));
    return true;
}

bool RendererFrontend::RemoveTexture(rn_texture* texture) {
    if (!ResolveHandle(_owner, texture)) return false;
    delete _textures.Remove(texture);
    return true;
}

bool RendererFrontend::UploadTexture(const rn_texture_upload_desc& desc) {
    if (!Open() || !ResolveHandle(_owner, desc.texture)) return false;
    PendingTextureUpload upload;
    if (!PrepareTextureUpload(*desc.texture, desc, upload)) return false;
    _pending.TextureUploads.push_back(std::move(upload));
    return true;
}

bool RendererFrontend::RegisterPipeline(const rn_pipeline_desc& desc, rn_pipeline*& out) {
    if (!Open()) return false;
    auto& runtime = _renderer.Runtime();
    auto impl = ImmediatePipeline::Create(runtime.GetDevice(), _renderer.Programs(), desc, runtime.GetPipelineCache());
    if (!impl) {
        SettleDeviceLoss();
        return false;
    }
    auto pipeline = radray::make_unique<rn_pipeline>();
    pipeline->Impl = std::move(impl);
    out = _pipelines.Insert(_owner, std::move(pipeline));
    return true;
}

bool RendererFrontend::RemovePipeline(rn_pipeline* pipeline) {
    if (!ResolveHandle(_owner, pipeline)) return false;
    if (_enqueued > _completed.load()) _retired.push_back({_enqueued, std::move(pipeline->Impl)});
    delete _pipelines.Remove(pipeline);
    return true;
}

bool RendererFrontend::CreateBuffer(const rn_buffer_desc& desc, rn_buffer_result& out) {
    auto& device = _renderer.Runtime().GetDevice();
    if (!BufferPool::Valid(desc, device.GetCapabilities().Limits.MaxBufferSize)) return false;
    SampleProgress();
    if (!Open()) return false;
    const auto usage = desc.usage & ~RN_BUFFER_CLIENT_STORAGE;
    auto buffer = _renderer.Buffers().Take(usage, desc.size, _completed.load());
    if (!buffer) buffer = BufferPool::Allocate(device, usage, desc.size);
    if (!buffer) {
        SettleDeviceLoss();
        return false;
    }
    if (IsReadbackBuffer(*buffer)) {
        buffer->ReadbackHost = radray::IntrusivePtr<HostReadStorage>(new HostReadStorage(desc.size));
        buffer->Mapped = buffer->ReadbackHost->Bytes.data();
    } else if ((usage & RN_BUFFER_MAP_WRITE) && !(usage & (RN_BUFFER_COPY_DST | RN_BUFFER_FENCE_ROTATED))) {
        buffer->ShadowHost.resize(desc.size);
        buffer->Mapped = buffer->ShadowHost.data();
    } else {
        buffer->Mapped = IsHostVisibleBuffer(*buffer) ? buffer->Gpu->GetMappedData().Get() : nullptr;
    }
    if (desc.initial) {
        if (buffer->Mapped) std::memcpy(buffer->Mapped, desc.initial, desc.size);
        if (IsHostVisibleBuffer(*buffer) && !buffer->Gpu->FlushMappedRange({0, desc.size})) {
            SettleDeviceLoss();
            return false;
        }
        if (buffer->Gpu && !IsHostVisibleBuffer(*buffer)) {
            auto* bytes = static_cast<const radray::byte*>(desc.initial);
            _pending.BufferUploads.push_back({buffer->Gpu, {bytes, bytes + desc.size}});
        }
    }
    out.mapped = buffer->Mapped;
    out.buffer = _buffers.Insert(_owner, std::move(buffer));
    return true;
}

bool RendererFrontend::RemoveBuffer(rn_buffer* buffer) {
    if (!ResolveHandle(_owner, buffer)) return false;
    if (buffer->ReadbackHost) buffer->ReadbackHost->Alive.store(false);
    _renderer.Buffers().Retire(radray::unique_ptr<rn_buffer>(_buffers.Remove(buffer)), _enqueued);
    return true;
}

void RendererFrontend::ApplyModelRemovals() {
    for (auto id : _pending.RemovedModelMeshes) _renderer.RemoveModelMesh(id);
    _pending.RemovedModelMeshes.clear();
}

void RendererFrontend::Present(rn_present_outcome outcome) {
    _outcome.store(outcome);
    if (outcome == RN_PRESENTED || outcome == RN_PRESENTED_SUBOPTIMAL) ++_presented;
    const auto recovery = _recovery.load();
    if (outcome == RN_PRESENT_SURFACE_LOST || (outcome == RN_PRESENT_RECREATE && recovery != RN_PRESENT_SURFACE_LOST) ||
        (outcome == RN_PRESENTED_SUBOPTIMAL && !recovery)) {
        _recovery.store(outcome);
    }
}

void RendererFrontend::SurfaceConfigured() {
    ++_generation;
    _recovery.store(0);
}

void RendererFrontend::SurfaceLost() {
    Present(RN_PRESENT_SURFACE_LOST);
}

uint32_t RendererFrontend::Drain(std::span<rn_batch_telemetry> out) {
    std::lock_guard lock(_telemetryMutex);
    const auto count = std::min(out.size(), _telemetry.size());
    for (size_t i = 0; i < count; ++i) {
        out[i] = _telemetry.front();
        _telemetry.pop_front();
    }
    return static_cast<uint32_t>(count);
}
}  // namespace raynium
