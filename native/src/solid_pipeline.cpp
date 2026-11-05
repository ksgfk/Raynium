#include <raynium/solid_pipeline.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::render;
namespace {
constexpr uint64_t StagingCapacity=8u<<20;
constexpr uint64_t InstanceCapacity=64u<<10;
}
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
    const bool timestamps=d.GetCapabilities().Features.TimestampQueries;
    std::array<SlotResources,2> slots;
    for(auto& slot:slots) {
        slot.passConstants=CreateBuffer(d,256,MemoryType::Upload,BufferUse::CBuffer);
        slot.passParameterSet=d.CreateShaderParameterSet({resources->PassGroupLayout()}).Release();
        if(!slot.passConstants || !slot.passParameterSet ||
           !slot.passParameterSet->Set(0,0,ShaderBufferBinding{slot.passConstants.get(),{0,256}}) || !slot.passParameterSet->FlushWrites()) {
            error="Cannot create frame slot pass bindings"; return false;
        }
        if(!MappedUploadBuffer::Create(d,StagingCapacity,BufferUse::CopySource,slot.staging,error) ||
           !MappedUploadBuffer::Create(d,InstanceCapacity,BufferUse::Vertex,slot.instances,error)) return false;
        if(timestamps) {
            slot.timestamps=d.CreateQueryPool({QueryType::Timestamp,2,"Raynium SOLID frame"}).Release();
            slot.timing=CreateBuffer(d,16,MemoryType::ReadBack,BufferUse::CopyDestination|BufferUse::MapRead);
            if(!slot.timestamps || !slot.timing) { error="Cannot create terrain timing resources"; return false; }
        }
    }
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(&d,std::move(resources),std::move(slots),std::move(completion)));
    return true;
}
bool SolidFramePipeline::ReadyForFrame(const RenderTarget& target,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.WaitReusable");
    if(_submitted==UINT64_MAX) { error="Terrain completion sequence exhausted"; return false; }
    const auto value=_slotCompletion[_submitted%2];
    if(value && !_completion->WaitFor(value,5000)) { error="Frame slot completion timed out; resources remain live"; return false; }
    auto& slot=_slots[_submitted%2];
    if(slot.depth && slot.depthWidth==target.Width() && slot.depthHeight==target.Height()) return true;
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.CreateDepth");
        auto depth=_device->CreateTexture({.Dim=TextureDimension::Dim2D,.Width=target.Width(),.Height=target.Height(),
            .DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,.Format=TextureFormat::D32_FLOAT,.Memory=MemoryType::Device,
            .Usage=TextureUse::DepthStencilWrite}).Release();
        auto view=depth ? _device->CreateTextureView({depth.get(),TextureDimension::Dim2D,TextureFormat::D32_FLOAT,{0,1,0,1},
            TextureViewUsage::DepthWrite}).Release() : nullptr;
        if(!depth || !view) { error="Cannot create terrain depth"; return false; }
        // Release the old view before its texture so nothing outlives the object it references.
        slot.depthView.reset(); slot.depth.reset();
        slot.depth=std::move(depth); slot.depthView=std::move(view);
        slot.depthWidth=target.Width(); slot.depthHeight=target.Height(); slot.depthInitialized=false;
    }
    return true;
}
SlotUploads SolidFramePipeline::NextUploads() noexcept {
    auto& slot=_slots[_submitted%2];
    return SlotUploads{*slot.staging,*slot.instances};
}
bool SolidFramePipeline::Finish(radray::string& error) {
    if(_submitted && !_completion->WaitFor(_submitted,5000)) { error="Renderer completion timed out; resources remain live"; return false; }
    return true;
}
void SolidFramePipeline::Extract(const radray::Scene& source,radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Extract");
    if(!_pending || slot.GetIndex()>=_frames.size()) RADRAY_ABORT("Missing terrain frame extraction source");
    if(slot.GetIndex()!=_submitted%2) RADRAY_ABORT("Terrain frame slot does not match the prepared slot resources");
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
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.StoreFrameData");
            if(!_pending->retiredResources.empty()) slot.RetireResource(std::move(_pending->retiredResources));
            _frames[slot.GetIndex()]=slot.GetArena().Emplace<FrameData>(std::move(*_pending));
        }
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
        const bool hasInstances=f.instance_count!=0;
        const bool timing=(f.flags&RN_FRAME_PROFILE)!=0 && resources.timestamps && resources.timing;
        auto command=slot.AcquireCommandBuffer();
        if(!command) RADRAY_ABORT("Cannot acquire terrain command buffer");
        auto& c=*command;
        std::array<Fence*,3> signals{_completion.get(),target.Ready(),input.textures ? input.textures->Released() : nullptr};
        std::array<uint64_t,3> signalValues{_submitted+1,f.output_sequence,f.input_sequence+1};
        std::array<Fence*,2> waits{}; std::array<uint64_t,2> waitValues{}; size_t waitCount=0;
        if(target.Submitted()) { waits[waitCount]=target.Consumed(); waitValues[waitCount++]=target.Submitted(); }
        if(input.textures) { waits[waitCount]=input.textures->Available(); waitValues[waitCount++]=f.input_sequence; }
        {
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.BeginCommands");
            c.Begin();
        }
        {
            auto end=radray::MakeScopeGuard([&]() noexcept {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.EndCommands");
                c.End();
            });
            RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordCommands");
            if(timing) { c.ResetQueryPool(resources.timestamps.get(),0,2); c.WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Top,0}); }
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordGeometryUploads");
                // D3D12 widens buffer barriers to the whole resource, so each destination transitions once.
                struct Destination { Buffer* target; BufferStates after; uint64_t beginByte,lastByte; };
                std::vector<Destination> destinations;
                std::vector<ResourceBarrierDescriptor> barriers;
                uint64_t stagingEnd=0;
                destinations.reserve(input.uploads.size());
                for(const auto& upload:input.uploads) {
                    if(!upload.bytes) continue;
                    const uint64_t last=upload.destinationOffset+upload.bytes;
                    const auto found=std::find_if(destinations.begin(),destinations.end(),
                        [&](const Destination& value) { return value.target==upload.destination; });
                    if(found==destinations.end()) destinations.push_back({upload.destination,upload.after,upload.destinationOffset,last});
                    else { found->beginByte=std::min(found->beginByte,upload.destinationOffset); found->lastByte=std::max(found->lastByte,last); }
                    stagingEnd=std::max(stagingEnd,upload.stagingOffset+upload.bytes);
                }
                if(!destinations.empty()) {
                    auto* staging=resources.staging->Get();
                    barriers.reserve(destinations.size()+1);
                    for(const auto& destination:destinations) barriers.emplace_back(BarrierBufferDescriptor{.Target=destination.target,
                        .Before=BufferState::Undefined,.After=BufferState::CopyDestination,
                        .Range={destination.beginByte,destination.lastByte-destination.beginByte}});
                    barriers.emplace_back(BarrierBufferDescriptor{.Target=staging,.Before=BufferState::HostWrite,
                        .After=BufferState::CopySource,.Range={0,stagingEnd}});
                    c.ResourceBarrier(barriers);
                    for(const auto& upload:input.uploads) if(upload.bytes)
                        c.CopyBufferToBuffer(upload.destination,upload.destinationOffset,staging,upload.stagingOffset,upload.bytes);
                    barriers.clear();
                    for(const auto& destination:destinations) barriers.emplace_back(BarrierBufferDescriptor{.Target=destination.target,
                        .Before=BufferState::CopyDestination,.After=destination.after,
                        .Range={destination.beginByte,destination.lastByte-destination.beginByte}});
                    barriers.emplace_back(BarrierBufferDescriptor{.Target=staging,.Before=BufferState::CopySource,
                        .After=BufferState::HostWrite,.Range={0,stagingEnd}});
                    c.ResourceBarrier(barriers);
                }
            }
            if(input.textures) {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.AcquireTextures");
                input.textures->Acquire(c,f.changed_textures);
            }
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.AttachmentBarriers");
                BufferBarrier(c,resources.passConstants.get(),BufferState::HostWrite,BufferState::CBuffer);
                if(hasInstances) BufferBarrier(c,resources.instances->Get(),BufferState::HostWrite,BufferState::Vertex);
                // Buffers decay to COMMON after each submission; textures without simultaneous access do not.
                TextureBarrier(c,resources.depth.get(),resources.depthInitialized ? TextureState::DepthWrite : TextureState::Undefined,
                    TextureState::DepthWrite);
                TextureBarrier(c,target.Color(),target.Submitted() ? TextureState::Common : TextureState::Undefined,TextureState::RenderTarget,
                    target.Submitted() ? ExternalOwnershipTransfer::Acquire : ExternalOwnershipTransfer::None);
            }
            resources.depthInitialized=true;
            RenderPassColorAttachment color{target.ColorView(),LoadAction::Clear,StoreAction::Store};
            std::copy_n(f.fog_color,4,color.Clear.Value.begin());
            auto encoder=c.BeginRenderPass({.Colors={&color,1},.DepthStencil=RenderPassDepthStencilAttachment{
                .View=resources.depthView.get(),.DepthLoad=LoadAction::Clear,.DepthStore=StoreAction::Store,.Clear={0,0}},
                .Width=target.Width(),.Height=target.Height(),.Name="Raynium SolidRaster"});
            if(!encoder) RADRAY_ABORT("Cannot begin terrain render pass");
            {
                auto endPass=radray::MakeScopeGuard([&]() noexcept {
                    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.EndRenderPass");
                    c.EndRenderPass(encoder.Release());
                });
                encoder->SetViewport({0,0,float(target.Width()),float(target.Height()),0,1});
                encoder->SetScissor({0,0,target.Width(),target.Height()});
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordDraws");
                auto& draws=slot.GetPassDrawList({0});
                if(!draws.VisibleCommands.empty()) {
                    const radray::MeshShaderParameterBinding pass{.GroupIndex=0,.ParameterSet=resources.passParameterSet.get()};
                    radray::MeshPassSubmitArgs args{.ParameterSets=std::span{&pass,1}};
                    if(hasInstances) args.InstanceStream=VertexBufferBinding{1,{resources.instances->Get(),0,
                        uint64_t(f.instance_count)*sizeof(rn_instance)}};
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
            if(hasInstances) BufferBarrier(c,resources.instances->Get(),BufferState::Vertex,BufferState::HostWrite);
            if(timing) {
                RADRAY_PROFILE_SCOPE_N("Raynium.Frame.ResolveTimestamps");
                BufferBarrier(c,resources.timing.get(),BufferState::Undefined,BufferState::CopyDestination);
                c.WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Bottom,1});
                c.ResolveQueryData({resources.timestamps.get(),0,2,resources.timing.get(),0});
                BufferBarrier(c,resources.timing.get(),BufferState::CopyDestination,BufferState::HostRead);
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
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.WaitFence");
        _completion->Wait(frame->completion);
    }
    auto& resources=_slots[slot.GetIndex()];
    if((frame->input.parameters.flags&RN_FRAME_PROFILE) && resources.timestamps && resources.timing) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.ReadGpuTiming");
        auto* ticks=static_cast<uint64_t*>(resources.timing->Map(0,16));
        if(ticks) {
            resources.timing->InvalidateMappedRange({0,16});
            const double sample=double(ticks[1]-ticks[0])*resources.timestamps->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs/1e6;
            _gpuSamples[_gpuSampleCount++]=sample;
            RADRAY_PROFILE_PLOT("Raynium.GpuMilliseconds", sample);
            resources.timing->Unmap();
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
