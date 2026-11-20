#include <raynium/modular_frame_pipeline.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/frame_upload.h>
#include <raynium/frame_record.h>
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
void ModularFramePipeline::PublishCompleted(uint64_t sequence) {
    for(auto& slot:_slots) slot.timestamps.Publish(sequence);
    // Two in-flight batches may target overlapping host bytes; publish them in submission order.
    size_t first=_slots[1].readbacks.Sequence()<_slots[0].readbacks.Sequence()?1:0;
    _slots[first].readbacks.Publish(sequence);
    _slots[1-first].readbacks.Publish(sequence);
}
bool ModularFramePipeline::WaitSubmit(Device& device,uint64_t sequence,int64_t timeoutNs,rn_wait_result& result) {
    result={};
    if(sequence>_submitted) {
        RADRAY_ERR_LOG("Unknown submission sequence {}",sequence);
        return false;
    }
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();
    const uint64_t limitMs=timeoutNs>0 ? uint64_t((timeoutNs-1)/1000000)+1 : 0;
    for(;;) {
        result.completed_sequence=_completion->GetCompletedValue();
        if(device.IsDeviceLost()) {
            result.flags=RN_SUBMIT_FLAG_DEVICE_LOST;
            return false;
        }
        PublishCompleted(result.completed_sequence);
        if(result.completed_sequence>=sequence || timeoutNs==0) return true;
        uint64_t sliceMs=1000;
        if(timeoutNs>0) {
            const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count());
            if(elapsed>=limitMs) return true;
            sliceMs=std::min(sliceMs,limitMs-elapsed);
        }
        _completion->WaitFor(sequence,sliceMs);
    }
}

void ModularFramePipeline::Finish(Device& device) {
    while(_submitted && !device.IsDeviceLost() && !_completion->WaitFor(_submitted,1000)) {}
}
bool ModularFramePipeline::PollCapture(uint64_t sequence,rn_capture_info& info,void* pixels,uint64_t capacity) {
    radray::ReadbackResultInfo result{};
    if(!_readback.Poll(sequence,_completion->GetCompletedValue(),result,static_cast<radray::byte*>(pixels),capacity)) {
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
    if(!slot.GetDevice().IsDeviceLost()) PublishCompleted(sequence);
    sequence=0;
}
void ModularFramePipeline::OnGpuTimingCompleted(radray::FrameSlot& slot,const radray::RDGGpuTimingSnapshot& snapshot) noexcept {
    _pendingGpuNs+=snapshot.TotalNanoseconds;
    if(!_slots[slot.GetIndex()].endsFrame) return;
    _gpuSequence=snapshot.UserSequence;
    _gpuNanoseconds=std::exchange(_pendingGpuNs,0);
    RADRAY_PROFILE_PLOT("Raynium.GpuMilliseconds",double(_gpuNanoseconds)/1e6);
}
}
