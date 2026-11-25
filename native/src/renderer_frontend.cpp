#include <raynium/renderer_frontend.h>
#include <radray/profiler.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace raynium {
RendererFrontend::RendererFrontend(rn_renderer* owner,RenderChannel& session,Renderer& renderer)
    :_owner(owner),_session(session),_renderer(renderer) {
    _session.SetIdlePoll([this] { return !_readbacks.empty() && Collect() && !_readbacks.empty(); });
}
RendererFrontend::~RendererFrontend() { _session.SetIdlePoll({}); }
bool RendererFrontend::Open() { return !_flags.load(std::memory_order_acquire); }
void RendererFrontend::Snapshot(rn_wait_result& out) {
    out.flags=_flags.load();
    out.completed_sequence=_completed.load();
    out.submitted_sequence=_submitted.load();
    out.consumed_sequence=out.flags?_enqueued:_consumed.load();
    out.presented_frames=_presented.load();
    out.surface_generation=_generation.load();
    out.surface_recovery_state=_recovery.load();
    out.last_present_outcome=_outcome.load();
    if(_hasWrites.load(std::memory_order_acquire)) {
        std::lock_guard lock(_publishMutex);
        while(!_writes.empty() && _writes.front().sequence<=out.completed_sequence) {
            for(const auto& write:_writes.front().writes) write.Apply();
            _writes.pop_front();
        }
        _hasWrites.store(!_writes.empty());
    }
}
void RendererFrontend::Advance(uint64_t done) {
    const auto submitted=_submitted.load(std::memory_order_acquire);
    auto next=std::min({done,submitted,_ceiling.load(std::memory_order_acquire)});
    auto old=_completed.load();
    while(old<next && !_completed.compare_exchange_weak(old,next)) {}
}
void RendererFrontend::SettleDeviceLoss() {
    if(_renderer.Runtime().GetDevice().IsDeviceLost() && Open())
        _session.Barrier([this] { return Collect(true); });
}
void RendererFrontend::SampleProgress() {
    if(!Open()) return;
    auto done=_renderer.Pipeline().SampleFence();
    if(_renderer.Runtime().GetDevice().IsDeviceLost()) { SettleDeviceLoss(); return; }
    Advance(done);
    while(!_retired.empty() && _retired.front().sequence<=_completed.load()) _retired.pop_front();
}
bool RendererFrontend::FailInput(rn_submit_result& out) {
    RADRAY_ERR_LOG("Invalid frame submission");
    _session.Barrier([this] { return Collect(true); });
    Snapshot(out.progress);
    return false;
}
bool RendererFrontend::Submit(const rn_batch_desc& desc,Swapchain* target,rn_submit_result& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.Freeze");
    SampleProgress();
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.QueueCapacity");
        _session.WaitUntil([&] { return _session.HasCapacity() || !Open(); });
    }
    if(!Open()) { Snapshot(out.progress); return false; }
    auto& envelope=_envelopes[_enqueued%2];
    envelope.Reset();
    if(!envelope.Freeze(desc)) return FailInput(out);
    for(auto range:std::span(desc.dirty_ranges,desc.dirty_count)) {
        auto* buffer=ResolveHandle(_owner,range.buffer);
        if(!buffer || !buffer->Contains(range.offset,range.length) || !(buffer->usage&RN_BUFFER_MAP_WRITE) ||
           (buffer->usage&RN_BUFFER_COPY_DST)) return FailInput(out);
        if(buffer->upload) {
            if(!buffer->upload->FlushMappedRange({range.offset,range.length})) return FailInput(out);
        } else {
            auto* copy=static_cast<radray::byte*>(envelope.Memory().allocate(range.length,1));
            std::memcpy(copy,buffer->shadowHost.data()+range.offset,range.length);
            envelope.shadows.push_back({buffer->device,range.offset,{copy,size_t(range.length)}});
        }
    }
    for(auto query:std::span(desc.queries,desc.query_count)) {
        if(query.reserved || !ResolveHandle(_owner,query.pool) || query.slot>=query.pool->storage->slots.size()) return FailInput(out);
    }
    const auto seq=_enqueued+1;
    if(desc.capture_count && !_renderer.Pipeline().RegisterCaptures({desc.captures,desc.capture_count},seq)) return FailInput(out);
    for(auto query:std::span(desc.queries,desc.query_count)) {
        auto& pool=*query.pool->storage;
        std::lock_guard lock(pool.mutex);
        pool.slots[query.slot]={seq,0,false};
    }
    std::swap(envelope.resources,_pending);
    envelope.frontend=this;
    envelope.sequence=_enqueued=out.sequence=seq;
    envelope.target=target;
#ifdef RADRAY_ENABLE_PROFILER
    if((desc.flags&RN_BATCH_MEASURE) && seq%128==0) ZoneTextF("%llu",static_cast<unsigned long long>(seq));
#endif
    _session.Publish(envelope);
    Snapshot(out.progress);
    _renderer.Buffers().Advance(seq);
    return true;
}
void RendererFrontend::Consume(BatchEnvelope& envelope) {
    if(!Open()) {
        if(envelope.batch.capture_count)
            _renderer.Pipeline().CancelUnsubmittedCaptures({envelope.batch.captures,envelope.batch.capture_count});
        return;
    }
    RADRAY_PROFILE_SCOPE_N("Raynium.Worker.Frame");
    FrameExecution frame{};
    auto& batch=envelope.batch;
    const auto seq=envelope.sequence;
    bool ok=_renderer.SubmitFrame(_owner,envelope,frame,[&](uint64_t submitted) {
        if(seq!=submitted) RADRAY_ABORT("Render mailbox sequence mismatch");
        if(batch.query_count || _renderer.Pipeline().HasInFlightReadbacks(seq)) {
            _readbacks.push_back(seq);
            _ceiling.store(_readbacks.front()-1);
        }
        _consumed.store(seq);
        _submitted.store(seq);
        _session.NotifyProgress();
    },[this] { return OnSlotAvailable(); });
    if(ok && batch.surface) Present(frame.outcome);
    if(!ok) Collect(true);
}
bool RendererFrontend::Wait(uint64_t sequence,int64_t timeout,rn_wait_result& out) {
    const auto start=std::chrono::steady_clock::now();
    auto remaining=[&]() -> int64_t {
        if(timeout<0) return -1;
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        return std::max<int64_t>(0,timeout-elapsed);
    };
    SampleProgress();
    if(sequence>_enqueued) { Snapshot(out); return false; }
    if(timeout && Open() && sequence>_submitted.load()) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.WaitForSubmission");
        _session.WaitUntil([&] { return !Open() || sequence<=_submitted.load(); },remaining());
    }
    while(timeout && Open() && sequence>_completed.load() && sequence<=_submitted.load()) {
        auto left=remaining();
        if(!left) break;
        constexpr uint64_t slice=2'000'000;
        RADRAY_PROFILE_SCOPE_N("Raynium.Frontend.WaitForGpu");
        auto status=_renderer.Pipeline().WaitFence(sequence,left<0?slice:std::min<uint64_t>(slice,left));
        if(status==radray::gpu::FenceStatus::DeviceLost) { SettleDeviceLoss(); break; }
        if(status==radray::gpu::FenceStatus::Completed) {
            Advance(sequence);
            _session.WaitUntil([&] { return !Open() || sequence<=_completed.load(); },remaining());
            break;
        }
        _session.PumpSentMessages();
    }
    Snapshot(out);
    return !out.flags;
}
bool RendererFrontend::Flush(rn_wait_result& out) {
    _session.Barrier([&] {
        if(Open()) {
            _renderer.Finish();
            _renderer.Runtime().GetQueue().Wait();
            Collect(_renderer.IsTerminal());
        }
        return true;
    });
    Snapshot(out);
    return !out.flags && out.completed_sequence==_enqueued;
}
bool RendererFrontend::Collect(bool terminal) {
    if(!Open()) return false;
    auto& pipe=_renderer.Pipeline();
    auto& device=_renderer.Runtime().GetDevice();
    const auto done=pipe.SampleFence();
    while(!device.IsDeviceLost() && !_readbacks.empty() && _readbacks.front()<=done) {
        const auto seq=_readbacks.front();
        radray::vector<PublishedTimestamp> timestamps;
        radray::vector<HostBufferWrite> writes;
        if(!pipe.Collect(seq,writes,timestamps) || device.IsDeviceLost()) { terminal=true; break; }
        for(auto& value:timestamps) {
            std::lock_guard lock(value.pool->mutex);
            auto& slot=value.pool->slots[value.slot];
            if(slot.sequence==value.sequence) slot={value.sequence,value.ticks,true};
        }
        if(!writes.empty()) {
            std::lock_guard lock(_publishMutex);
            _writes.push_back({seq,std::move(writes)});
            _hasWrites.store(true);
        }
        _readbacks.pop_front();
        _ceiling.store(_readbacks.empty()?UINT64_MAX:_readbacks.front()-1);
    }
    const bool lost=device.IsDeviceLost();
    if(!lost) Advance(done);
    OnSlotAvailableTelemetry();
    if(terminal || lost) _flags.store(RN_SUBMIT_FLAG_TERMINAL_ERROR|(lost?RN_SUBMIT_FLAG_DEVICE_LOST:0));
    _session.NotifyProgress();
    return Open();
}
void RendererFrontend::OnSlotAvailableTelemetry() {
    rn_batch_telemetry report{};
    if(!_renderer.Pipeline().PopTelemetry(report)) return;
    std::lock_guard lock(_telemetryMutex);
    do { _telemetry.push_back(report); } while(_renderer.Pipeline().PopTelemetry(report));
}
bool RendererFrontend::OnSlotAvailable() {
    if(!_readbacks.empty()) return Collect();
    OnSlotAvailableTelemetry();
    return Open();
}
void RendererFrontend::DeviceFailure() { Collect(true); }
bool RendererFrontend::PollCapture(uint64_t id,rn_capture_info& info,void* pixels,uint64_t capacity) {
    SampleProgress();
    return _renderer.Pipeline().PollCapture(id,_completed.load(std::memory_order_acquire),Open(),info,pixels,capacity);
}
bool RendererFrontend::CreateQueryPool(uint32_t count,rn_query_pool*& out) {
    auto& runtime=_renderer.Runtime();
    if(!Open() || !count || !runtime.GetDevice().GetCapabilities().Features.TimestampQueries || !runtime.GetQueue().GetTimestampValidBits()) return false;
    auto pool=radray::make_unique<rn_query_pool>();
    pool->storage=radray::IntrusivePtr<QueryPoolStorage>(new QueryPoolStorage(count));
    out=_queries.Insert(_owner,std::move(pool));
    return true;
}
bool RendererFrontend::RemoveQueryPool(rn_query_pool* pool) {
    if(!ResolveHandle(_owner,pool)) return false;
    delete _queries.Remove(pool);
    return true;
}
bool RendererFrontend::Query(rn_query_pool* pool,uint32_t first,std::span<rn_query_sample> samples,rn_wait_result& out) {
    SampleProgress();
    Snapshot(out);
    if(!ResolveHandle(_owner,pool)) return false;
    auto& storage=*pool->storage;
    std::lock_guard lock(storage.mutex);
    if(first>storage.slots.size() || samples.size()>storage.slots.size()-first) return false;
    for(size_t i=0;i<samples.size();++i) {
        const auto& slot=storage.slots[first+i];
        samples[i]={slot.available && slot.sequence<=out.completed_sequence?slot.sequence:0,slot.ticks};
    }
    return true;
}
bool RendererFrontend::CreateTexture(const rn_texture_desc& desc,rn_texture*& out) {
    auto& device=_renderer.Runtime().GetDevice();
    if(!Open() || !ValidateTextureDesc(device.GetCapabilities(),desc)) return false;
    auto gpu=CreateSceneTexture(device,desc);
    if(!gpu) { SettleDeviceLoss(); return false; }
    auto texture=radray::make_unique<rn_texture>();
    texture->desc=desc;
    texture->texture=std::move(gpu);
    out=_textures.Insert(_owner,std::move(texture));
    return true;
}
bool RendererFrontend::RemoveTexture(rn_texture* texture) {
    if(!ResolveHandle(_owner,texture)) return false;
    delete _textures.Remove(texture);
    return true;
}
bool RendererFrontend::UploadTexture(const rn_texture_upload_desc& desc) {
    if(!Open() || !ResolveHandle(_owner,desc.texture)) return false;
    PendingTextureUpload upload;
    if(!PrepareTextureUpload(*desc.texture,desc,upload)) return false;
    _pending.textureUploads.push_back(std::move(upload));
    return true;
}
bool RendererFrontend::RegisterPipeline(const rn_pipeline_desc& desc,rn_pipeline*& out) {
    if(!Open()) return false;
    auto& runtime=_renderer.Runtime();
    auto impl=ImmediatePipeline::Create(runtime.GetDevice(),_renderer.Programs(),desc,runtime.GetPipelineCache());
    if(!impl) { SettleDeviceLoss(); return false; }
    auto pipeline=radray::make_unique<rn_pipeline>();
    pipeline->impl=std::move(impl);
    out=_pipelines.Insert(_owner,std::move(pipeline));
    return true;
}
bool RendererFrontend::RemovePipeline(rn_pipeline* pipeline) {
    if(!ResolveHandle(_owner,pipeline)) return false;
    if(_enqueued>_completed.load()) _retired.push_back({_enqueued,std::move(pipeline->impl)});
    delete _pipelines.Remove(pipeline);
    return true;
}
bool RendererFrontend::CreateBuffer(const rn_buffer_desc& desc,rn_buffer_result& out) {
    auto& device=_renderer.Runtime().GetDevice();
    if(!BufferPool::Valid(desc,device.GetCapabilities().Limits.MaxBufferSize)) return false;
    SampleProgress();
    if(!Open()) return false;
    const auto usage=desc.usage&~RN_BUFFER_CLIENT_STORAGE;
    auto buffer=_renderer.Buffers().Take(usage,desc.size,_completed.load());
    if(!buffer) buffer=BufferPool::Allocate(device,usage,desc.size);
    if(!buffer) { SettleDeviceLoss(); return false; }
    if(buffer->IsReadback()) {
        buffer->readbackHost=radray::IntrusivePtr<HostReadStorage>(new HostReadStorage(desc.size));
        buffer->mapped=buffer->readbackHost->bytes.data();
    } else if((usage&RN_BUFFER_MAP_WRITE) && !(usage&(RN_BUFFER_COPY_DST|RN_BUFFER_FENCE_ROTATED))) {
        buffer->shadowHost.resize(desc.size);
        buffer->mapped=buffer->shadowHost.data();
    } else buffer->mapped=buffer->upload?buffer->upload->GetMappedData():nullptr;
    if(desc.initial) {
        if(buffer->mapped) std::memcpy(buffer->mapped,desc.initial,desc.size);
        if(buffer->upload && !buffer->upload->FlushMappedRange({0,desc.size})) { SettleDeviceLoss(); return false; }
        if(buffer->device) {
            auto* bytes=static_cast<const radray::byte*>(desc.initial);
            _pending.bufferUploads.push_back({buffer->device,{bytes,bytes+desc.size}});
        }
    }
    out.mapped=buffer->mapped;
    out.buffer=_buffers.Insert(_owner,std::move(buffer));
    return true;
}
bool RendererFrontend::RemoveBuffer(rn_buffer* buffer) {
    if(!ResolveHandle(_owner,buffer)) return false;
    if(buffer->readbackHost) buffer->readbackHost->alive.store(false);
    _renderer.Buffers().Retire(radray::unique_ptr<rn_buffer>(_buffers.Remove(buffer)),_enqueued);
    return true;
}
void RendererFrontend::ApplyModelRemovals() {
    for(auto id:_pending.removedModelMeshes) _renderer.RemoveModelMesh(id);
    _pending.removedModelMeshes.clear();
}
void RendererFrontend::Present(rn_present_outcome outcome) {
    _outcome.store(outcome);
    if(outcome==RN_PRESENTED || outcome==RN_PRESENTED_SUBOPTIMAL) ++_presented;
    const auto recovery=_recovery.load();
    if(outcome==RN_PRESENT_SURFACE_LOST || (outcome==RN_PRESENT_RECREATE && recovery!=RN_PRESENT_SURFACE_LOST) ||
       (outcome==RN_PRESENTED_SUBOPTIMAL && !recovery)) _recovery.store(outcome);
}
void RendererFrontend::SurfaceConfigured() { ++_generation; _recovery.store(0); }
void RendererFrontend::SurfaceLost() { Present(RN_PRESENT_SURFACE_LOST); }
uint32_t RendererFrontend::Drain(std::span<rn_batch_telemetry> out) {
    std::lock_guard lock(_telemetryMutex);
    auto count=std::min(out.size(),_telemetry.size());
    for(size_t i=0;i<count;++i) { out[i]=_telemetry.front(); _telemetry.pop_front(); }
    return static_cast<uint32_t>(count);
}
}
