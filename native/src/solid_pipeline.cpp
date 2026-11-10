#include <raynium/solid_pipeline.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace raynium {
using namespace radray::render;
namespace {
constexpr uint64_t StagingCapacity=8u<<20;
constexpr uint64_t InstanceCapacity=64u<<10;
}
void SolidPassProcessor::AddMeshBatch(const radray::MeshBatchContext& context,radray::MeshPassDrawListContext& output) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Cache.BuildBatch");
    if(!_activeMaterialBindings) { _hasErrors=true; return; }
    const auto binding=_activeMaterialBindings->find(context.Batch.Material.GetIndex());
    if(binding==_activeMaterialBindings->end() || !binding->second || !binding->second->parameters ||
       context.Batch.Topology!=PrimitiveTopology::TriangleList) { _hasErrors=true; return; }
    const radray::MeshShaderParameterBinding group1{.GroupIndex=1,.ParameterSet=binding->second->parameters.get()};
    BuildMeshDrawCommands(context,{.Pipeline=_resources.Pipeline(),
        .ParameterSets=std::span{&group1,1},.AllowDeferredInstanceStream=true,.AllowElementMerging=true},output);
}
bool SolidFramePipeline::Create(RayniumRuntime& device,std::span<const radray::byte> shaders,radray::unique_ptr<SolidFramePipeline>& out,radray::string& error) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Pipeline.Create");
    if(out) { error="Frame pipeline output occupied"; return false; }
    auto& d=device.GetDevice();
    radray::unique_ptr<MeshPassResources> resources;
    if(!MeshPassResources::Create(d,shaders,resources,error)) return false;
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
        slot.staging=radray::BatchedBufferUpload::Create(d,StagingCapacity).Release();
        slot.instances=radray::MappedUploadBuffer::Create(d,InstanceCapacity,BufferUse::Vertex).Release();
        if(!slot.staging || !slot.instances) { error="Cannot create frame slot uploads"; return false; }
        if(timestamps) {
            slot.timestamps=d.CreateQueryPool({QueryType::Timestamp,2,"Raynium SOLID frame"}).Release();
            slot.timing=CreateBuffer(d,16,MemoryType::ReadBack,BufferUse::CopyDestination|BufferUse::MapRead);
            if(!slot.timestamps || !slot.timing) { error="Cannot create terrain timing resources"; return false; }
        }
    }
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(device,std::move(resources),std::move(slots),std::move(completion)));
    return true;
}
SlotUploads SolidFramePipeline::GetUploads(radray::FrameSlot& frame) noexcept {
    auto& slot=_slots[frame.GetIndex()];
    slot.staging->Reset();
    return SlotUploads{frame,*slot.staging,*slot.instances,slot.lists};
}
bool SolidFramePipeline::Finish(radray::string& error) {
    if(_submitted && !_completion->WaitFor(_submitted,5000)) { error="Renderer completion timed out; resources remain live"; return false; }
    return true;
}
void SolidFramePipeline::ExtractFrameData(const radray::Scene&,radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Extract");
    if(!_pending || slot.GetIndex()>=_frames.size()) RADRAY_ABORT("Missing terrain frame extraction source");
    if(_processor.HasErrors()) RADRAY_ABORT("Invalid SOLID mesh command or material binding");
    auto* frame=slot.GetArena().Emplace<FrameData>(std::move(*_pending));
    _frames[slot.GetIndex()]=frame;
}
void SolidFramePipeline::SetupGraph(radray::RDGBuilder& graph,const radray::Scene&,radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SetupGraph");
    const auto index=slot.GetIndex();
    auto& resources=_slots[index];
    const auto& input=_frames[index]->input;
    const auto& f=input.parameters;
    const auto constants=graph.ImportBuffer(resources.passConstants.get(),BufferState::HostWrite,BufferState::HostWrite);
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Graph.PassConstants");
        struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4]; } data{};
        static_assert(sizeof(Constants)==192);
        std::copy_n(f.projection,16,data.projection); std::copy_n(f.view,16,data.view); std::copy_n(f.camera_block,3,data.block);
        data.block[3]=(f.flags&RN_FRAME_RGSS)!=0; std::copy_n(f.camera_fraction,3,data.fraction);
        data.fraction[3]=slot.GetDevice().GetBackend()==RenderBackend::Vulkan ? 1.f : -1.f;
        std::copy_n(f.fog_color,4,data.fog); std::copy_n(f.fog_distances,4,data.distances);
        std::array<std::byte,256> bytes{}; std::memcpy(bytes.data(),&data,sizeof(data));
        graph.UploadBuffer(constants,0,bytes);
    }
    const auto instances=graph.ImportBuffer(resources.instances->Get(),BufferState::HostWrite,BufferState::HostWrite);
    if(!resources.staging->CommitToGraph(graph,"Raynium GeometryUpload")) RADRAY_ABORT("Cannot commit terrain uploads");
    radray::vector<radray::RDGTextureHandle> textures;
    if(input.textures) textures=input.textures->SetupGraph(graph,f.changed_textures);
    const bool submitted=input.target->Submitted()!=0;
    const auto color=graph.ImportTexture(input.target->Color(),submitted ? TextureState::Common : TextureState::Undefined,
        TextureState::Common,{.Acquire=submitted,.Release=true});
    const auto depth=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=input.target->Width(),.Height=input.target->Height(),
        .DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,.Format=TextureFormat::D32_FLOAT,.Memory=MemoryType::Device,
        .Usage=TextureUse::DepthStencilWrite});
    graph.MarkOutput(color);
    graph.AddRasterPass("Raynium SolidRaster",[&](radray::RDGRasterPassBuilder& pass) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Graph.RasterAccesses");
        ColorClearValue clear{}; std::copy_n(f.fog_color,4,clear.Value.begin());
        pass.UseMeshPass({0}).ReadBuffer(constants,BufferState::CBuffer)
            .WriteColor(0,color,LoadAction::Clear,StoreAction::Store,clear,input.target->ColorView())
            .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Clear,StoreAction::Discard,
                LoadAction::DontCare,StoreAction::Discard,{0,0});
        if(f.instance_count) pass.ReadBuffer(instances,BufferState::Vertex);
        input.vertices->AddPassReadAccess(graph,pass);
        input.indices->AddPassReadAccess(graph,pass);
        if(!input.visibleBatches.empty()) for(const auto texture:textures) pass.ReadTexture(texture);
    },[this,index](radray::RDGRasterPassContext& context) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.RecordDraws");
        const auto& input=_frames[index]->input;
        const auto& resources=_slots[index];
        auto& encoder=context.GetEncoder();
        encoder.SetViewport({0,0,float(input.target->Width()),float(input.target->Height()),0,1});
        encoder.SetScissor({0,0,input.target->Width(),input.target->Height()});
        const radray::MeshShaderParameterBinding pass{.GroupIndex=0,.ParameterSet=resources.passParameterSet.get()};
        radray::MeshPassSubmitArgs args{.ParameterSets=std::span{&pass,1}};
        if(input.parameters.instance_count) args.InstanceStream=VertexBufferBinding{1,{resources.instances->Get(),0,
            uint64_t(input.parameters.instance_count)*sizeof(rn_instance)}};
        return context.GetSlot().GetPassDrawList({0}).Submit(encoder,args);
    });
}
void SolidFramePipeline::ExtractMeshPass(radray::MeshPassId pass,const radray::Scene& source,radray::FrameSlot& slot,
    radray::MeshPassFrameDrawList& draws) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.GatherVisible");
    if(pass.Value!=0 || !source.GatherVisibleMeshDrawCommands(pass,_frames[slot.GetIndex()]->input.visibleBatches,draws))
        RADRAY_ABORT("Cannot gather cached SOLID command pointers");
    if(auto cache=source.GetPassCache(pass)) RADRAY_PROFILE_PLOT("Raynium.Cache.Commands",double(cache->GetCommands().size()));
    RADRAY_PROFILE_PLOT("Raynium.DrawCommands",double(draws.VisibleCommands.size()));
}
void SolidFramePipeline::SubmitFrame(radray::FrameSlot& slot,CommandBuffer& graphCommands) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SubmitHandoff");
    if(_submitted==std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Terrain completion sequence exhausted");
    auto& frame=*_frames[slot.GetIndex()];
    auto& input=frame.input; const auto& f=input.parameters; auto& target=*input.target;
    auto& resources=_slots[slot.GetIndex()];
    const bool timing=(f.flags&RN_FRAME_PROFILE)!=0 && resources.timestamps && resources.timing;
    std::array<CommandBuffer*,3> commands{&graphCommands}; size_t commandCount=1;
    if(timing) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.ResolveTimestamps");
        auto before=slot.AcquireCommandBuffer(); auto after=slot.AcquireCommandBuffer();
        if(!before || !after) RADRAY_ABORT("Cannot acquire timestamp command buffers");
        // Timestamp brackets include every graph ownership barrier and all native GPU work.
        before->Begin();
        before->ResetQueryPool(resources.timestamps.get(),0,2);
        before->WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Top,0});
        before->End();
        after->Begin();
        BufferBarrier(*after,resources.timing.get(),BufferState::Undefined,BufferState::CopyDestination);
        after->WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Bottom,1});
        after->ResolveQueryData({resources.timestamps.get(),0,2,resources.timing.get(),0});
        BufferBarrier(*after,resources.timing.get(),BufferState::CopyDestination,BufferState::HostRead);
        after->End();
        commands={before.Get(),&graphCommands,after.Get()}; commandCount=commands.size();
    }
    std::array<Fence*,3> signals{_completion.get(),target.Ready(),input.textures ? input.textures->Released() : nullptr};
    std::array<uint64_t,3> signalValues{_submitted+1,f.output_sequence,f.input_sequence+1};
    std::array<Fence*,2> waits{}; std::array<uint64_t,2> waitValues{}; size_t waitCount=0;
    if(target.Submitted()) { waits[waitCount]=target.Consumed(); waitValues[waitCount++]=target.Submitted(); }
    if(input.textures) { waits[waitCount]=input.textures->Available(); waitValues[waitCount++]=f.input_sequence; }
    const size_t signalCount=input.textures ? 3 : 2;
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.QueueSubmit");
        slot.GetQueue().Submit({.CmdBuffers={commands.data(),commandCount},.SignalFences={signals.data(),signalCount},.SignalValues={signalValues.data(),signalCount},
            .WaitFences={waits.data(),waitCount},.WaitValues={waitValues.data(),waitCount}});
    }
    frame.completion=++_submitted;
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
