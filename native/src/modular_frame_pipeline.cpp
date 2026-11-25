#include <raynium/modular_frame_pipeline.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/frame_upload.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <algorithm>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::gpu;
bool ModularFramePipeline::Create(radray::DeviceContext& device,std::span<const radray::byte> shaders,bool indirect,radray::unique_ptr<ModularFramePipeline>& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Pipeline.Create");
    if(out) RADRAY_ABORT("Frame pipeline output occupied");
    auto& d=device.GetDevice();
    auto cache=device.GetPipelineCache();
    radray::unique_ptr<MeshPassResources> resources;
    if(!MeshPassResources::Create(d,shaders,resources,cache)) return false;
    radray::unique_ptr<OutputPassResources> output;
    if(!OutputPassResources::Create(d,shaders,output,cache)) return false;
    radray::unique_ptr<RenderProgramLibrary> scene;
    if(!RenderProgramLibrary::Create(shaders,scene)) return false;
    auto completion=d.CreateFence().Release();
    if(!completion) { RADRAY_ERR_LOG("Cannot create terrain completion fence"); return false; }
    out=radray::unique_ptr<ModularFramePipeline>(new ModularFramePipeline(device,std::move(resources),
        std::move(output),std::move(scene),std::move(completion),indirect));
    return true;
}
bool ModularFramePipeline::HasInFlightReadbacks(uint64_t sequence) const noexcept {
    return std::ranges::any_of(_slots,[sequence](const auto& slot) { return slot.readbacks.Sequence()==sequence; });
}
bool ModularFramePipeline::RegisterCaptures(std::span<const uint64_t> ids,uint64_t sequence) {
    if(ids.empty()) return true;
    std::lock_guard lock(_readbackMutex);
    if(ids.size()>4096 || _readback.GetActiveRequestCount()+_unreservedCaptures.size()>4096-ids.size()) return false;
    size_t inserted=0;
    for(auto id:ids) {
        if(!id || _readback.HasRequest(id) || !_unreservedCaptures.emplace(id,sequence).second) {
            for(size_t i=0;i<inserted;++i) _unreservedCaptures.erase(ids[i]);
            return false;
        }
        ++inserted;
    }
    return true;
}
bool ModularFramePipeline::ReserveCapture(uint64_t id,uint32_t width,uint32_t height) {
    std::lock_guard lock(_readbackMutex);
    auto it=_unreservedCaptures.find(id);
    if(it==_unreservedCaptures.end()) return false;
    if(!_readback.ReserveTexture(id,{width,height,TextureFormat::RGBA8_UNORM,1})) return false;
    _unreservedCaptures.erase(it);
    return true;
}
void ModularFramePipeline::CancelUnsubmittedCaptures(std::span<const uint64_t> ids) noexcept {
    std::lock_guard lock(_readbackMutex);
    for(auto id:ids) _unreservedCaptures.erase(id);
    _readback.CancelUnsubmitted();
}
bool ModularFramePipeline::PollCapture(uint64_t id,uint64_t completed,bool open,rn_capture_info& info,void* dst,uint64_t capacity) {
    std::lock_guard lock(_readbackMutex);
    if(_unreservedCaptures.contains(id)) {
        info={sizeof(info)};
        return open;
    }
    radray::ReadbackResultInfo result{};
    const auto status=_readback.Poll(id,completed,result,static_cast<radray::byte*>(dst),capacity);
    if(status==radray::ReadbackStatus::Ready) {
        info={sizeof(info),1,result.Texture.Width,result.Texture.Height,result.PackedByteSize,uint32_t(result.PackedRowPitch),0};
        return true;
    }
    if(status==radray::ReadbackStatus::Pending) {
        info={sizeof(info)};
        return open;
    }
    return false;
}
bool ModularFramePipeline::Collect(uint64_t sequence,radray::vector<HostBufferWrite>& writes,radray::vector<PublishedTimestamp>& timestamps) {
    for(auto& slot:_slots) {
        if(!slot.timestamps.Collect(sequence,timestamps) || !slot.readbacks.Collect(sequence,writes)) return false;
    }
    return true;
}
void ModularFramePipeline::Finish(Device& device) {
    while(_submitted && !device.IsDeviceLost() && WaitFence(_submitted,1'000'000'000ull)==FenceStatus::Timeout) {}
}
void ModularFramePipeline::ExtractFrameData(radray::FrameSlot&) noexcept {}
void ModularFramePipeline::SetupGraph(radray::RDGBuilder& graph,radray::FrameSlot& slot) noexcept {
    auto& resources=_slots[slot.GetIndex()];
    resources.timestamps.Reset(graph);
    const auto& detail=slot.GetDevice().GetDetail();
    FrameTextureImports imports{graph,slot.GetScratchMemoryResource(),&detail};
    _pending->uploads->AddUploads(graph,imports,_pending->shadowWrites);
    _tape->Replay(graph,imports,_readback,*_output,resources.timestamps,resources.readbacks,_pending->color);
    resources.timestamps.Resolve(graph);
}
bool ModularFramePipeline::SubmitFrame(radray::FrameSlot& slot,CommandBuffer& graphCommands) noexcept {
    if(_submitted==UINT64_MAX) RADRAY_ABORT("Submission sequence exhausted");
    const auto& input=*_pending;
    CommandBuffer* commands[]{&graphCommands};
    radray::array<Fence*,1> signals{_completion.get()};
    radray::array<uint64_t,1> signalValues{_submitted+1};
    SwapChainSyncObject* acquire[]{input.waitToDraw}; SwapChainSyncObject* present[]{input.readyToPresent};
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.QueueSubmit");
        if(!slot.GetQueue().Submit({.CmdBuffers=commands,.SignalFences=signals,.SignalValues=signalValues,
            .WaitToExecute={acquire,input.waitToDraw?1u:0u},.ReadyToPresent={present,input.readyToPresent?1u:0u}})) return false;
    }
    _inFlight[slot.GetIndex()]=++_submitted;
    if(_hasCaptures) _readback.MarkSubmitted(_submitted);
    _slots[slot.GetIndex()].timestamps.Submitted(_submitted);
    _slots[slot.GetIndex()].readbacks.Submitted(_submitted);
    return true;
}
void ModularFramePipeline::Wait(radray::FrameSlot& slot) noexcept {
    auto& sequence=_inFlight[slot.GetIndex()];
    if(!sequence) return;
    _completion->Wait(sequence);
    sequence=0;
}
void ModularFramePipeline::MeasureBatch(radray::FrameSlot& slot, bool enabled) {
    SetGpuProfiling(slot,enabled?radray::RDGGpuProfilingMode::FrameOnly:radray::RDGGpuProfilingMode::Off,_submitted+1);
}
void ModularFramePipeline::RecordTelemetry(rn_batch_telemetry report) {
    // The worker records CPU data before BeginFrame or WaitIdle can retire this slot.
    _inFlightTelemetry.push_back({report});
}
void ModularFramePipeline::OnFrameSlotRetired(radray::FrameSlot&,const radray::RDGGpuTimingSnapshot& snapshot) noexcept {
    for(auto& item:_inFlightTelemetry) {
        if(item.report.sequence!=snapshot.UserSequence) continue;
        if(snapshot.Status==radray::RDGGpuTimingStatus::Valid) {
            item.report.gpu_ns=snapshot.TotalNanoseconds;
            item.report.flags=RN_TELEMETRY_HAS_GPU_NS;
        }
        item.retired=true;
        break;
    }
    while(!_inFlightTelemetry.empty() && _inFlightTelemetry.front().retired) {
        _readyTelemetry.push_back(_inFlightTelemetry.front().report);
        _inFlightTelemetry.pop_front();
    }
}
bool ModularFramePipeline::PopTelemetry(rn_batch_telemetry& report) {
    if(_readyTelemetry.empty()) return false;
    report=_readyTelemetry.front();
    _readyTelemetry.pop_front();
    return true;
}
}
