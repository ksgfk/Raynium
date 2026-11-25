#include <raynium/frame_queue.h>
#include <radray/profiler.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace raynium {

FrameQueue::FrameQueue(RenderThreadSession& session, Renderer& renderer) : _session(session), _renderer(renderer),
    _maxBufferSize(renderer.Runtime().GetDevice().GetCapabilities().Limits.MaxBufferSize),
    _timestamps(renderer.Runtime().GetDevice().GetCapabilities().Features.TimestampQueries && renderer.Runtime().GetQueue().GetTimestampValidBits()) {
    _progress.last_present_outcome = RN_PRESENT_NONE;
    _session.SetIdlePoll([this] { return Poll(); });
}
FrameQueue::~FrameQueue() { _session.SetIdlePoll({}); }

bool FrameQueue::Open() {
    std::lock_guard lock(_mutex);
    return !_closing;
}

void FrameQueue::SnapshotLocked(rn_wait_result& out) {
    for (const auto& write : _writes) {
        auto buffer = _buffers.find(write.id);
        if (buffer != _buffers.end())
            std::memcpy(buffer->second.host.data() + write.offset, write.bytes.data(), write.bytes.size());
    }
    _writes.clear();
    out = _progress;
}

void FrameQueue::Snapshot(rn_wait_result& out) {
    std::lock_guard lock(_mutex);
    SnapshotLocked(out);
}

void FrameQueue::FailInput(rn_submit_result& out) {
    RADRAY_ERR_LOG("Invalid frame packet");
    {
        std::lock_guard lock(_mutex);
        _closing = true;
    }
    _session.Barrier([this] { return Collect(true); });
    Snapshot(out.progress);
}

bool FrameQueue::Submit(const rn_batch_desc& desc, Swapchain* target, rn_submit_result& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.Freeze");
    auto packet = radray::shared_ptr<FramePacket>(FramePacket::Create(desc));
    if (!packet) { FailInput(out); return false; }
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.QueueCapacity");
        _session.WaitUntil([&] {
            std::lock_guard lock(_mutex);
            return !_queued || _closing;
        });
    }
    radray::vector<HostBufferWrite> uploads;
    {
        std::unique_lock lock(_mutex);
        if (_closing) { SnapshotLocked(out.progress); return false; }
        bool valid = packet->captures.size() <= 4096 - _captures.size();
        radray::unordered_set<uint64_t> ids;
        for (auto id : packet->captures) valid &= !_captures.contains(id) && ids.insert(id).second;
        for (auto query : packet->queries) {
            auto it = _queries.find(query.pool);
            valid &= it != _queries.end() && query.slot < it->second.size();
        }
        for (auto range : std::span(packet->batch.dirty_ranges, packet->batch.dirty_count)) {
            auto it = _buffers.find(range.id);
            if (it == _buffers.end() || range.reserved || !range.length || range.offset > it->second.size ||
                range.length > it->second.size - range.offset || !(it->second.usage & RN_BUFFER_MAP_WRITE) ||
                (it->second.usage & RN_BUFFER_COPY_DST)) { valid = false; break; }
            if (!(it->second.usage & RN_BUFFER_FENCE_ROTATED)) {
                const auto* start = it->second.host.data() + range.offset;
                uploads.push_back({range.id, range.offset, {start, start + range.length}});
            }
        }
        if (!valid) { lock.unlock(); FailInput(out); return false; }
        out.sequence = ++_enqueued;
        for (auto id : packet->captures) _captures.emplace(id, Capture{out.sequence});
        for (auto q : packet->queries) _queries.at(q.pool)[q.slot] = {out.sequence, 0, false};
        _queued = true;
        SnapshotLocked(out.progress);
    }
#ifdef RADRAY_ENABLE_PROFILER
    // Match sparse submit markers to Java TerrainFrame.sequence across the two recording clocks.
    if ((desc.flags & RN_BATCH_MEASURE) && out.sequence % 128 == 0) ZoneTextF("%llu",static_cast<unsigned long long>(out.sequence));
#endif
    _session.Post([this, target, seq=out.sequence, packet=std::move(packet), uploads=std::move(uploads)]() mutable {
        {
            std::lock_guard lock(_mutex);
            _queued = false;
            if (_closing) { _session.NotifyProgress(); return; }
        }
        _session.NotifyProgress();
        RADRAY_PROFILE_SCOPE_N("Raynium.Worker.Frame");
        for (auto& write : uploads) {
            if (!_renderer.Buffers().UploadShadow(write.id, write.offset, std::move(write.bytes))) { Collect(true); return; }
        }
        FrameExecution result{};
        bool ok = _renderer.SubmitFrame(packet->batch, target, result, [&](uint64_t submitted) {
            if (submitted != seq) RADRAY_ABORT("Render queue sequence mismatch");
            _pending.push_back({seq, packet->captures, packet->queries});
            std::lock_guard lock(_mutex);
            _progress.consumed_sequence = _progress.submitted_sequence = seq;
            _session.NotifyProgress();
        }, [this] { return Collect(); });
        if (packet->batch.surface && !target && ok) result.outcome = RN_PRESENT_SURFACE_LOST;
        if (ok && packet->batch.surface) Present(result.outcome);
        if (!ok) Collect(true);
    });
    return true;
}

bool FrameQueue::Wait(uint64_t sequence, int64_t timeout, rn_wait_result& out) {
    {
        std::lock_guard lock(_mutex);
        if (sequence > _enqueued) { SnapshotLocked(out); return false; }
        if (!timeout || sequence <= _progress.completed_sequence || _progress.flags) {
            SnapshotLocked(out);
            return !out.flags;
        }
    }
    // A blocked owner cannot post the next frame to wake the idle worker.
    // Let the GPU signal this wait as soon as the requested submission finishes.
    const auto waitLimit = std::chrono::nanoseconds(timeout < 0 ? 1'000'000'000 : std::min<int64_t>(timeout,1'000'000'000));
    const auto until = std::chrono::steady_clock::now() + waitLimit;
    _session.Post([this,sequence,until] {
        auto& pipeline = _renderer.Pipeline();
        if (!Open() || sequence <= pipeline.CompletedSequence() || sequence > pipeline.SubmittedSequence()) return;
        RADRAY_PROFILE_SCOPE_N("Raynium.Worker.RequestedFenceWait");
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        if (remaining.count() > 0) pipeline.WaitForFence(sequence,static_cast<uint64_t>(remaining.count()));
        Collect();
    });
    _session.WaitUntil([&] {
        std::lock_guard lock(_mutex);
        return _progress.completed_sequence >= sequence || _progress.flags;
    }, timeout);
    Snapshot(out);
    return !out.flags;
}

bool FrameQueue::Flush(rn_wait_result& out) {
    _session.Barrier([&] {
        if (Open()) {
            _renderer.Finish();
            _renderer.Runtime().GetQueue().Wait();
            Collect(_renderer.IsTerminal());
        }
        return true;
    });
    Snapshot(out);
    return !out.flags && out.completed_sequence >= _enqueued;
}

bool FrameQueue::Collect(bool terminal) {
    {
        std::lock_guard lock(_mutex);
        if (_progress.flags) return false;
        terminal |= _closing;
    }
    auto& device = _renderer.Runtime().GetDevice();
    auto& pipe = _renderer.Pipeline();
    const auto done = pipe.SampleFence();
    struct QueryValue { PacketQuery query; rn_query_sample value; };
    radray::vector<QueryValue> values;
    radray::vector<std::pair<uint64_t,Capture>> captures;
    radray::vector<HostBufferWrite> writes;
    auto safe = pipe.CompletedSequence();
    while (!device.IsDeviceLost() && !_pending.empty() && _pending.front().sequence <= done) {
        const auto& batch = _pending.front();
        auto begin = captures.size();
        bool ok = true;
        for (auto id : batch.captures) {
            auto& capture = captures.emplace_back(id, Capture{batch.sequence}).second;
            if (!pipe.PollCapture(id, done, capture.info, nullptr, 0) || !capture.info.ready) { ok = false; break; }
            capture.pixels.resize(capture.info.byte_size);
            if (!pipe.PollCapture(id, done, capture.info, capture.pixels.data(), capture.pixels.size())) { ok = false; break; }
        }
        if (!ok || device.IsDeviceLost()) { captures.resize(begin); terminal = true; break; }
        auto writeBegin = writes.size();
        pipe.Collect(batch.sequence, writes);
        if (device.IsDeviceLost()) { writes.resize(writeBegin); captures.resize(begin); terminal = true; break; }
        for (auto q : batch.queries) values.push_back({q, _renderer.Query(q.pool, q.slot)});
        safe = batch.sequence;
        _pending.pop_front();
    }
    const bool lost = device.IsDeviceLost();
    terminal |= lost;
    rn_batch_telemetry report{};
    {
        std::lock_guard lock(_mutex);
        for (auto& [id, capture] : captures) _captures.at(id) = std::move(capture);
        for (const auto& [q, value] : values) {
            auto it = _queries.find(q.pool);
            if (it != _queries.end() && it->second[q.slot].sequence == value.sequence && value.sequence)
                it->second[q.slot] = {value.sequence,value.ticks,true};
        }
        for (auto& write : writes) _writes.push_back(std::move(write));
        while (pipe.PopTelemetry(report)) _telemetry.push_back(report);
        _progress.completed_sequence = safe;
        if (terminal) {
            _closing = true;
            _progress.flags = RN_SUBMIT_FLAG_TERMINAL_ERROR | (lost ? RN_SUBMIT_FLAG_DEVICE_LOST : 0);
            _progress.consumed_sequence = _enqueued;
            std::erase_if(_captures, [safe](const auto& pair) { return pair.second.sequence > safe; });
        }
    }
    _session.NotifyProgress();
    return !terminal;
}

bool FrameQueue::Poll() {
    if (_pending.empty()) return false;
    return Collect() && !_pending.empty();
}
void FrameQueue::DeviceFailure() { Collect(true); }

bool FrameQueue::PollCapture(uint64_t id, rn_capture_info& info, void* pixels, uint64_t capacity) {
    std::lock_guard lock(_mutex);
    auto it = _captures.find(id);
    if (it == _captures.end()) return false;
    if (!it->second.info.ready) { info = {sizeof(info)}; return true; }
    if (pixels && capacity < it->second.pixels.size()) return false;
    info = it->second.info;
    if (pixels) {
        std::memcpy(pixels, it->second.pixels.data(), it->second.pixels.size());
        _captures.erase(it);
    }
    return true;
}

bool FrameQueue::CreateQueryPool(uint32_t count, uint32_t& id) {
    std::lock_guard lock(_mutex);
    if (_closing || !_timestamps || !count || !_nextQueryPool) return false;
    id = _nextQueryPool++;
    _queries.try_emplace(id, count);
    _session.Post([this,id,count] { if (Open()) _renderer.RegisterQueryPool(id,count); });
    return true;
}

bool FrameQueue::CreateTexture(const rn_uploaded_texture_desc& desc) {
    return _session.Barrier([&] {
        if (!Open()) return false;
        if (!_renderer.CreateTexture(desc)) {
            if (_renderer.IsTerminal()) DeviceFailure();
            return false;
        }
        std::lock_guard lock(_mutex);
        _textures.emplace(desc.id,desc);
        return true;
    });
}
bool FrameQueue::RemoveTexture(uint32_t id) {
    std::lock_guard lock(_mutex);
    if (!_textures.erase(id)) return false;
    _session.Post([this,id] { _renderer.DestroyTexture(id); });
    return true;
}
bool FrameQueue::UploadTexture(const rn_texture_upload_desc& desc) {
    rn_uploaded_texture_desc texture;
    {
        std::lock_guard lock(_mutex);
        auto it = _textures.find(desc.id);
        if (_closing || it == _textures.end()) return false;
        texture = it->second;
    }
    TextureRegistry::Upload upload;
    if (!TextureRegistry::PrepareUpload(texture,desc,upload)) return false;
    std::lock_guard lock(_mutex);
    if (_closing) return false;
    _session.Post([this,upload=std::move(upload)]() mutable {
        if (Open()) _renderer.UploadTexture(std::move(upload));
    });
    return true;
}
bool FrameQueue::RegisterPipeline(const rn_pipeline_desc& desc,uint32_t& id) {
    return _session.Barrier([&] {
        if (!Open()) return false;
        if (!_renderer.RegisterPipeline(desc,id)) {
            if (_renderer.IsTerminal()) DeviceFailure();
            return false;
        }
        std::lock_guard lock(_mutex);
        _pipelines.emplace(id);
        return true;
    });
}
bool FrameQueue::RemovePipeline(uint32_t id) {
    std::lock_guard lock(_mutex);
    if (!_pipelines.erase(id)) return false;
    _session.Post([this,id] { _renderer.DestroyPipeline(id); });
    return true;
}
bool FrameQueue::RemoveQueryPool(uint32_t id) {
    std::lock_guard lock(_mutex);
    return _queries.erase(id) != 0;
}
bool FrameQueue::Query(uint32_t id, uint32_t first, std::span<rn_query_sample> samples, rn_wait_result& out) {
    std::lock_guard lock(_mutex);
    SnapshotLocked(out);
    auto pool = _queries.find(id);
    if (pool == _queries.end() || first > pool->second.size() || samples.size() > pool->second.size() - first) return false;
    for (size_t i=0; i<samples.size(); ++i) {
        const auto& slot = pool->second[first+i];
        samples[i] = {slot.available ? slot.sequence : 0, slot.ticks};
    }
    return true;
}

bool FrameQueue::CreateBuffer(const rn_buffer_desc& desc, rn_buffer_result& result) {
    if(!BufferRegistry::Valid(desc,_maxBufferSize)) return false;
    const auto usage=desc.usage & ~RN_BUFFER_CLIENT_STORAGE;
    std::unique_lock lock(_mutex);
    if(_closing || !_nextBuffer) return false;
    radray::shared_ptr<BufferEntry> entry;
    if(usage&RN_BUFFER_MAP_READ) {
        entry=radray::make_shared<BufferEntry>();
        entry->usage=usage; entry->size=desc.size;
    } else {
        entry=_renderer.Buffers().Take(usage,desc.size,_progress.completed_sequence);
        if(!entry) {
            lock.unlock();
            if(!_session.Barrier([&] {
                if(!Open() || _renderer.IsTerminal()) return false;
                entry=BufferRegistry::Allocate(_renderer.Runtime().GetDevice(),usage,desc.size);
                if(!entry && _renderer.IsTerminal()) DeviceFailure();
                return bool(entry);
            })) return false;
            lock.lock();
            if(_closing) {
                _session.Post([discard=std::move(entry)]{});
                return false;
            }
        }
    }
    auto id=_nextBuffer++;
    auto& buffer=_buffers.emplace(id,FrontBuffer{usage,desc.size}).first->second;
    result={id,0,entry->mapped};
    if((usage&RN_BUFFER_MAP_READ) || ((usage&RN_BUFFER_MAP_WRITE) && !(usage&(RN_BUFFER_COPY_DST|RN_BUFFER_FENCE_ROTATED)))) {
        buffer.host.resize(desc.size);
        result.mapped=buffer.host.data();
    }
    if(result.mapped && desc.initial) std::memcpy(result.mapped,desc.initial,desc.size);
    bool flush=desc.initial && entry->upload;
    radray::vector<radray::byte> initial;
    if(desc.initial && entry->device) {
        auto first=static_cast<const radray::byte*>(desc.initial);
        initial.assign(first,first+desc.size);
    }
    _session.Post([this,id,entry=std::move(entry),initial=std::move(initial),flush]() mutable {
        if(Open()) _renderer.Buffers().Register(id,std::move(entry),std::move(initial),flush);
    });
    return true;
}
bool FrameQueue::RemoveBuffer(uint32_t id) {
    std::lock_guard lock(_mutex);
    return _buffers.erase(id) != 0;
}

void FrameQueue::Present(rn_present_outcome outcome) {
    std::lock_guard lock(_mutex);
    _progress.last_present_outcome = outcome;
    if (outcome == RN_PRESENTED || outcome == RN_PRESENTED_SUBOPTIMAL) ++_progress.presented_frames;
    auto& recovery = _progress.surface_recovery_state;
    if (outcome == RN_PRESENT_SURFACE_LOST || (outcome == RN_PRESENT_RECREATE && recovery != RN_PRESENT_SURFACE_LOST) ||
        (outcome == RN_PRESENTED_SUBOPTIMAL && !recovery)) recovery = outcome;
}
void FrameQueue::SurfaceConfigured() {
    std::lock_guard lock(_mutex);
    ++_progress.surface_generation;
    _progress.surface_recovery_state = 0;
}
void FrameQueue::SurfaceLost() { Present(RN_PRESENT_SURFACE_LOST); }
bool FrameQueue::NeedsNewSurface() {
    std::lock_guard lock(_mutex);
    return _progress.surface_recovery_state == RN_PRESENT_SURFACE_LOST;
}
uint32_t FrameQueue::Drain(std::span<rn_batch_telemetry> out) {
    std::lock_guard lock(_mutex);
    auto count = std::min(out.size(), _telemetry.size());
    for (size_t i=0;i<count;++i) { out[i]=_telemetry.front(); _telemetry.pop_front(); }
    return static_cast<uint32_t>(count);
}
}
