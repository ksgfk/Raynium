#include <raynium/solid_pipeline.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::render;
void SolidPassProcessor::AddMeshBatch(const radray::MeshBatchContext& context) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Cache.BuildBatch");
    if(!_activeMaterialBindings) { _hasErrors=true; return; }
    const auto binding=_activeMaterialBindings->find(context.Batch.Material.Index);
    if(binding==_activeMaterialBindings->end() || !binding->second || !binding->second->parameters ||
       context.Batch.Topology!=PrimitiveTopology::TriangleList) { _hasErrors=true; return; }
    const radray::MeshShaderParameterBinding group1{.GroupIndex=1,.ParameterSet=binding->second->parameters.get()};
    const uint64_t key=(uint64_t(context.Batch.Material.Index)<<48)|(uint64_t(context.BatchHandle.Index)<<32);
    if(!BuildMeshDrawCommands(context,{.Pipeline=_resources.Pipeline(),.BaseSortKey=key,
        .ParameterSets=std::span{&group1,1},.AllowDeferredInstanceStream=true})) _hasErrors=true;
}
bool SolidFramePipeline::Create(RayniumRuntime& device,radray::unique_ptr<SolidFramePipeline>& out,radray::string& error) {
    if(out) { error="Frame pipeline output occupied"; return false; }
    auto& d=device.GetDevice();
    radray::unique_ptr<MeshPassResources> resources;
    if(!MeshPassResources::Create(d,resources,error)) return false;
    auto completion=d.CreateFence().Release();
    if(!completion) { error="Cannot create terrain completion fence"; return false; }
    std::array<SlotResources,2> slots;
    for(auto& slot:slots) {
        slot.passConstants=CreateBuffer(d,256,MemoryType::Upload,BufferUse::CBuffer);
        slot.passParameterSet=d.CreateShaderParameterSet({resources->PassGroupLayout()}).Release();
        if(!slot.passConstants || !slot.passParameterSet ||
           !slot.passParameterSet->Set(0,0,ShaderBufferBinding{slot.passConstants.get(),{0,256}}) || !slot.passParameterSet->FlushWrites()) {
            error="Cannot create frame slot pass bindings"; return false;
        }
    }
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(std::move(resources),std::move(slots),std::move(completion))); return true;
}
bool SolidFramePipeline::ReadyForFrame(radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.WaitReusable");
    if(_submitted==UINT64_MAX) { error="Terrain completion sequence exhausted"; return false; }
    const auto value=_slotCompletion[_submitted%2];
    if(value && !_completion->WaitFor(value,5000)) { error="Frame slot completion timed out; resources remain live"; return false; }
    return true;
}
bool SolidFramePipeline::Finish(radray::string& error) {
    if(_submitted && !_completion->WaitFor(_submitted,5000)) { error="Renderer completion timed out; resources remain live"; return false; }
    return true;
}
void SolidFramePipeline::Extract(const radray::Scene& source,radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Extract");
    if(!_pending || slot.GetIndex()>=_frames.size()) RADRAY_ABORT("Missing terrain frame extraction source");
    if(_processor.HasErrors()) RADRAY_ABORT("Invalid SOLID mesh command or material binding");
    try {
        auto& draws=slot.GetPassDrawList({0});
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.GatherVisible");
            if(!source.GatherVisibleMeshDrawCommands({0},_pending->visibleBatches,draws.VisibleCommands)) {
                RADRAY_ABORT("Cannot gather cached SOLID commands");
            }
        }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SortVisible");
            draws.SortVisibleCommands();
        }
        if(auto cache=source.GetCachedDrawList({0})) RADRAY_PROFILE_PLOT("Raynium.Cache.Commands", double(cache->Commands.Count()));
        RADRAY_PROFILE_PLOT("Raynium.DrawCommands", double(draws.VisibleCommands.size()));
        if(!_pending->retiredResources.empty()) slot.RetireResource(std::move(_pending->retiredResources));
        _frames[slot.GetIndex()]=slot.GetArena().Emplace<FrameData>(std::move(*_pending));
    } catch(...) { RADRAY_ABORT("Cannot allocate SOLID frame data"); }
}
void SolidFramePipeline::Record(radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Record");
    auto& frame=*_frames[slot.GetIndex()]; auto& input=frame.input;
    try {
        const auto& f=input.parameters; auto& target=*input.target; auto& resources=_slots[slot.GetIndex()];
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.UploadConstants");
            struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4]; } constants{};
            static_assert(sizeof(Constants)==192);
            std::copy_n(f.projection,16,constants.projection); std::copy_n(f.view,16,constants.view); std::copy_n(f.camera_block,3,constants.block);
            constants.block[3]=(f.flags&RN_FRAME_RGSS)!=0; std::copy_n(f.camera_fraction,3,constants.fraction);
            constants.fraction[3]=slot.GetDevice().GetBackend()==RenderBackend::Vulkan ? 1.f : -1.f;
            std::copy_n(f.fog_color,4,constants.fog); std::copy_n(f.fog_distances,4,constants.distances);
            std::array<std::byte,256> bytes{}; std::memcpy(bytes.data(),&constants,sizeof(constants));
            if(!WriteBuffer(*resources.passConstants,bytes.data(),bytes.size())) RADRAY_ABORT("Cannot upload pass constants");
        }
        auto command=slot.AcquireCommandBuffer();
        if(!command) RADRAY_ABORT("Cannot acquire terrain command buffer");
        auto& c=*command;
        std::array<Fence*,3> signals{_completion.get(),target.Ready(),input.textures ? input.textures->Released() : nullptr};
        std::array<uint64_t,3> signalValues{_submitted+1,f.output_sequence,f.input_sequence+1};
        std::array<Fence*,2> waits{}; std::array<uint64_t,2> waitValues{}; size_t waitCount=0;
        if(target.Submitted()) { waits[waitCount]=target.Consumed(); waitValues[waitCount++]=target.Submitted(); }
        if(input.textures) { waits[waitCount]=input.textures->Available(); waitValues[waitCount++]=f.input_sequence; }
        c.Begin();
        {
            auto end=radray::MakeScopeGuard([&]() noexcept { c.End(); });
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordCommands");
            if(input.timestamps) { c.ResetQueryPool(input.timestamps.get(),0,2); c.WriteTimestamp({input.timestamps.get(),QueryPipelineStage::Top,0}); }
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordGeometryUploads");
                for(const auto& upload:input.uploads) {
                    BufferBarrier(c,upload.destination,BufferState::Undefined,BufferState::CopyDestination);
                    BufferBarrier(c,upload.staging.get(),BufferState::HostWrite,BufferState::CopySource);
                    c.CopyBufferToBuffer(upload.destination,0,upload.staging.get(),0,upload.bytes);
                    BufferBarrier(c,upload.destination,BufferState::CopyDestination,upload.after);
                }
            }
            if(input.textures) {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.AcquireTextures");
                input.textures->Acquire(c,f.changed_textures);
            }
            BufferBarrier(c,resources.passConstants.get(),BufferState::HostWrite,BufferState::CBuffer);
            if(input.instances) BufferBarrier(c,input.instances.get(),BufferState::HostWrite,BufferState::Vertex);
            TextureBarrier(c,input.depth.get(),TextureState::Undefined,TextureState::DepthWrite);
            TextureBarrier(c,target.Color(),target.Submitted() ? TextureState::Common : TextureState::Undefined,TextureState::RenderTarget,
                target.Submitted() ? ExternalOwnershipTransfer::Acquire : ExternalOwnershipTransfer::None);
            RenderPassColorAttachment color{input.colorView.get(),LoadAction::Clear,StoreAction::Store};
            std::copy_n(f.fog_color,4,color.Clear.Value.begin());
            auto encoder=c.BeginRenderPass({.Colors={&color,1},.DepthStencil=RenderPassDepthStencilAttachment{
                .View=input.depthView.get(),.DepthLoad=LoadAction::Clear,.DepthStore=StoreAction::Store,.Clear={0,0}},
                .Width=target.Width(),.Height=target.Height(),.Name="Raynium SolidRaster"});
            if(!encoder) RADRAY_ABORT("Cannot begin terrain render pass");
            {
                auto endPass=radray::MakeScopeGuard([&]() noexcept { c.EndRenderPass(encoder.Release()); });
                encoder->SetViewport({0,0,float(target.Width()),float(target.Height()),0,1});
                encoder->SetScissor({0,0,target.Width(),target.Height()});
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordDraws");
                auto& draws=slot.GetPassDrawList({0});
                if(!draws.VisibleCommands.empty()) {
                    const radray::MeshShaderParameterBinding pass{.GroupIndex=0,.ParameterSet=resources.passParameterSet.get()};
                    const radray::MeshPassSubmitArgs args{.ParameterSets=std::span{&pass,1},
                        .InstanceStream=VertexBufferBinding{1,{input.instances.get(),0,input.instances->GetDesc().Size}}};
                    if(!draws.Submit(*encoder,slot.GetScene()->GetCachedDrawList({0}),args)) {
                        RADRAY_ABORT("Cannot submit cached SOLID commands");
                    }
                }
            }
            if(input.textures) {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.ReleaseTextures");
                input.textures->Release(c);
            }
            TextureBarrier(c,target.Color(),TextureState::RenderTarget,TextureState::Common,ExternalOwnershipTransfer::Release);
            BufferBarrier(c,resources.passConstants.get(),BufferState::CBuffer,BufferState::HostWrite);
            if(input.timestamps) {
                BufferBarrier(c,input.timing.get(),BufferState::Undefined,BufferState::CopyDestination);
                c.WriteTimestamp({input.timestamps.get(),QueryPipelineStage::Bottom,1});
                c.ResolveQueryData({input.timestamps.get(),0,2,input.timing.get(),0});
                BufferBarrier(c,input.timing.get(),BufferState::CopyDestination,BufferState::HostRead);
            }
        }
        CommandBuffer* commands[]{command.Get()}; const size_t signalCount=input.textures ? 3 : 2;
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.QueueSubmit");
            slot.GetQueue().Submit({.CmdBuffers=commands,.SignalFences={signals.data(),signalCount},.SignalValues={signalValues.data(),signalCount},
                .WaitFences={waits.data(),waitCount},.WaitValues={waitValues.data(),waitCount}});
        }
        frame.completion=++_submitted; _slotCompletion[slot.GetIndex()]=frame.completion;
    } catch(...) { RADRAY_ABORT("Cannot record SOLID frame"); }
}
void SolidFramePipeline::Wait(radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.WaitCompleted");
    auto* frame=_frames[slot.GetIndex()];
    if(!frame) return;
    _completion->Wait(frame->completion);
    auto& input=frame->input;
    if(input.timestamps) {
        auto* ticks=static_cast<uint64_t*>(input.timing->Map(0,16));
        if(ticks) {
            input.timing->InvalidateMappedRange({0,16});
            const double sample=double(ticks[1]-ticks[0])*input.timestamps->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs/1e6;
            _gpuSamples[_gpuSampleCount++]=sample;
            RADRAY_PROFILE_PLOT("Raynium.GpuMilliseconds", sample);
            input.timing->Unmap();
            if(_gpuSampleCount==_gpuSamples.size()) {
                std::sort(_gpuSamples.begin(),_gpuSamples.end());
                RADRAY_INFO_LOG("Raynium SOLID GPU samples=120 ms_p50={} p95={} p99={}",_gpuSamples[60],_gpuSamples[114],_gpuSamples[118]);
                _gpuSampleCount=0;
            }
        } else RADRAY_WARN_LOG("Cannot read completed terrain timestamps");
    }
    _frames[slot.GetIndex()]=nullptr;
}
}
