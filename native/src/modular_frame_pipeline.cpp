#include <raynium/modular_frame_pipeline.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/frame_upload.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
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
void ModularFramePipeline::Collect(uint64_t sequence, radray::vector<HostBufferWrite>& writes) {
    for(auto& slot:_slots) {
        slot.timestamps.Publish(sequence);
        slot.readbacks.Collect(sequence,writes);
    }
    _completed=sequence;
}

void ModularFramePipeline::Finish(Device& device) {
    while(_submitted && !device.IsDeviceLost() && !_completion->WaitFor(_submitted,1000)) {}
}
bool ModularFramePipeline::PollCapture(uint64_t sequence,uint64_t completed,rn_capture_info& info,void* pixels,uint64_t capacity) {
    radray::ReadbackResultInfo result{};
    if(!_readback.Poll(sequence,completed,result,static_cast<radray::byte*>(pixels),capacity)) {
        RADRAY_ERR_LOG("Cannot poll capture: id={} capacity={}",sequence,capacity); return false;
    }
    info={sizeof(info),result.Ready?1u:0u,result.Texture.Width,result.Texture.Height,result.PackedByteSize,uint32_t(result.PackedRowPitch),0};
    return true;
}
void ModularFramePipeline::ExtractFrameData(radray::FrameSlot&) noexcept {}
void ModularFramePipeline::SetupGraph(radray::RDGBuilder& graph,radray::FrameSlot& slot) noexcept {
    auto& resources=_slots[slot.GetIndex()];
    resources.timestamps.Reset(graph);
    _buffers->AddInitialUploads(graph);
    const auto& detail=slot.GetDevice().GetDetail();
    FrameTextureImports imports{graph,slot.GetScratchMemoryResource(),&detail};
    _pending->textures->AddUploads(graph,imports,nullptr);
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
    _readback.MarkSubmitted(_submitted);
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
    RetireSlot(slot.GetIndex());
    if(enabled) {
        const auto seq=_submitted+1;
        _slots[slot.GetIndex()].measuredSequence=seq;
        _measurements.try_emplace(seq);
    }
    SetGpuProfiling(slot,enabled?radray::RDGGpuProfilingMode::FrameOnly:radray::RDGGpuProfilingMode::Off,_submitted+1);
}
void ModularFramePipeline::OnGpuTimingCompleted(radray::FrameSlot&,const radray::RDGGpuTimingSnapshot& snapshot) noexcept {
    auto it=_measurements.find(snapshot.UserSequence);
    if(it==_measurements.end()) return;
    it->second.report.gpu_ns=snapshot.TotalNanoseconds;
    it->second.report.flags=RN_TELEMETRY_HAS_GPU_NS;
}
void ModularFramePipeline::RecordTelemetry(rn_batch_telemetry report) {
    _measurements.at(report.sequence).report=report;
}
void ModularFramePipeline::RetireSlot(uint32_t index) {
    auto seq=std::exchange(_slots[index].measuredSequence,0);
    if(auto it=_measurements.find(seq); it!=_measurements.end()) it->second.retired=true;
}
bool ModularFramePipeline::PopTelemetry(rn_batch_telemetry& report) {
    auto it=_measurements.begin();
    if(it==_measurements.end() || !it->second.retired) return false;
    report=it->second.report;
    _measurements.erase(it);
    return report.sequence!=0;
}
}
