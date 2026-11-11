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
bool SolidFramePipeline::Create(RayniumRuntime& device,std::span<const radray::byte> shaders,bool indirect,radray::unique_ptr<SolidFramePipeline>& out,radray::string& error) {
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
        slot.indirect=radray::MappedUploadBuffer::Create(d,64u<<10,BufferUse::Indirect).Release();
        if(!slot.staging || !slot.instances || !slot.indirect) { error="Cannot create frame slot uploads"; return false; }
        for(auto& parameters:slot.oitParameters) {
            parameters=d.CreateShaderParameterSet({resources->OitGroupLayout()}).Release();
            if(!parameters) { error="Cannot create OIT slot bindings"; return false; }
        }
        if(timestamps) {
            slot.timestamps=d.CreateQueryPool({QueryType::Timestamp,18,"Raynium terrain frame"}).Release();
            slot.timing=CreateBuffer(d,144,MemoryType::ReadBack,BufferUse::CopyDestination|BufferUse::MapRead);
            if(!slot.timestamps || !slot.timing) { error="Cannot create terrain timing resources"; return false; }
        }
    }
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(device,std::move(resources),std::move(slots),std::move(completion),indirect));
    return true;
}
SlotUploads SolidFramePipeline::GetUploads(radray::FrameSlot& frame) noexcept {
    auto& slot=_slots[frame.GetIndex()];
    slot.staging->Reset();
    for(auto& parameters:slot.oitParameters) parameters->ResetBindingsForReuse();
    return SlotUploads{frame,*slot.staging,*slot.instances,*slot.indirect,slot.lists,_indirect};
}
bool SolidFramePipeline::Finish(radray::string& error) {
    if(_submitted && !_completion->WaitFor(_submitted,5000)) { error="Renderer completion timed out; resources remain live"; return false; }
    return true;
}
void SolidFramePipeline::ExtractFrameData(radray::FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Extract");
    if(!_pending || slot.GetIndex()>=_frames.size()) RADRAY_ABORT("Missing terrain frame extraction source");
    auto* frame=slot.GetArena().Emplace<FrameData>(*_pending);
    _frames[slot.GetIndex()]=frame;
}
void SolidFramePipeline::SetupGraph(radray::RDGBuilder& graph,radray::FrameSlot& slot) noexcept {
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
    const auto indirect=_indirect && !input.lists->arguments.empty()
        ? graph.ImportBuffer(resources.indirect->Get(),BufferState::HostWrite,BufferState::HostWrite):radray::RDGBufferHandle{};
    if(!resources.staging->CommitToGraph(graph,"Raynium GeometryUpload")) RADRAY_ABORT("Cannot commit terrain uploads");
    radray::vector<radray::RDGTextureHandle> textures;
    if(input.textures) textures=input.textures->SetupGraph(graph,f.changed_textures);
    const bool submitted=input.target->Submitted()!=0;
    const auto color=graph.ImportTexture(input.target->Color(),submitted ? TextureState::Common : TextureState::Undefined,
        TextureState::Common,{.Acquire=submitted,.Release=true});
    const uint32_t width=input.target->Width(),height=input.target->Height();
    const auto makeTexture=[&](TextureFormat format,TextureUses usage) {
        return graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,
            .MipLevels=1,.SampleCount=1,.Format=format,.Memory=MemoryType::Device,.Usage=usage});
    };
    const auto depth=makeTexture(TextureFormat::D32_FLOAT,TextureUse::DepthStencilWrite|TextureUse::DepthStencilRead);
    graph.MarkOutput(color);
    using Handle=radray::RDGTextureHandle;
    using Handles=std::array<Handle,4>;
    const auto geometry=[&](uint32_t passId,Handle destination,Handle second,bool clear,bool writeDepth,Handles reads,int bindingIndex) {
        _frames[index]->timingMask|=1u<<passId;
        graph.AddRasterPass(TerrainPrograms[passId],[&](radray::RDGRasterPassBuilder& pass) {
            pass.ReadBuffer(constants,BufferState::CBuffer);
            if(indirect.IsValid()) pass.ReadBuffer(indirect,BufferState::Indirect);
            pass.WriteColor(0,destination,clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,
                passId==Solid?ColorClearValue{{f.fog_color[0],f.fog_color[1],f.fog_color[2],0}}:
                passId==OitBounds?ColorClearValue{{-std::numeric_limits<float>::max(),0,0,0}}:ColorClearValue{});
            if(second.IsValid()) pass.WriteColor(1,second,LoadAction::Clear,StoreAction::Store);
            pass.SetDepthStencil(depth,writeDepth?TextureState::DepthWrite:TextureState::DepthRead,
                passId==Solid?LoadAction::Clear:LoadAction::Load,StoreAction::Store,
                LoadAction::DontCare,StoreAction::Discard,{0,0});
            if(f.instance_count) pass.ReadBuffer(instances,BufferState::Vertex);
            input.vertices->AddPassReadAccess(graph,pass); input.indices->AddPassReadAccess(graph,pass);
            for(auto texture:textures) pass.ReadTexture(texture);
            for(auto texture:reads) if(texture.IsValid()) pass.ReadTexture(texture);
        },[this,index,passId,reads,bindingIndex,width,height](radray::RDGRasterPassContext& context) {
            auto& encoder=context.GetEncoder(); auto& slot=_slots[index];
            const bool timing=(_frames[index]->input.parameters.flags&RN_FRAME_PROFILE) && slot.timestamps;
            if(timing) encoder.GetCommandBuffer()->WriteTimestamp({slot.timestamps.get(),QueryPipelineStage::Top,2+passId*2});
            encoder.SetViewport({0,0,float(width),float(height),0,1}); encoder.SetScissor({0,0,width,height});
            RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Record");
            encoder.BindGraphicsPipelineState(_resources->Pipeline(passId));
            encoder.BindShaderParameterSet(0,slot.passParameterSet.get());
            uint64_t bindingChanges=2,directCalls=0,indirectCalls=0;
            if(bindingIndex>=0) {
                auto* parameters=slot.oitParameters[bindingIndex].get();
                for(uint32_t i=0;i<4;++i) if(!parameters->Set(i,0,context.ResolveView(reads[i].IsValid()?reads[i]:reads[0],TextureViewUsage::Resource).Get())) return false;
                if(!parameters->FlushWrites()) return false;
                encoder.BindShaderParameterSet(2,parameters); ++bindingChanges;
            }
            const auto& input=_frames[index]->input;
            const auto& lists=*input.lists;
            const auto layer=passId<=Translucent?passId:uint32_t(Translucent);
            const auto& draws=lists.layers[layer];
            _frames[index]->visibleCommands[passId]=draws.recordCount;
            const auto count=input.parameters.instance_count;
            if(count) {
                const VertexBufferBinding instances{1,{slot.instances->Get(),0,uint64_t(count)*sizeof(rn_instance)}};
                encoder.BindVertexBuffers(std::span{&instances,1}); ++bindingChanges;
            }
            const TerrainDrawBatch* previous=nullptr;
            for(const auto& batch:std::span{lists.batches}.subspan(draws.firstBatch,draws.batchCount)) {
                if(!previous || previous->material->parameters.get()!=batch.material->parameters.get()) {
                    encoder.BindShaderParameterSet(1,batch.material->parameters.get()); ++bindingChanges;
                }
                if(!previous || previous->vertices.Target!=batch.vertices.Target || previous->vertices.Offset!=batch.vertices.Offset || previous->vertices.Size!=batch.vertices.Size) {
                    const VertexBufferBinding vertices{0,batch.vertices};
                    encoder.BindVertexBuffers(std::span{&vertices,1}); ++bindingChanges;
                }
                if(!previous || previous->indices.Target!=batch.indices.Target || previous->indices.Offset!=batch.indices.Offset || previous->indices.Stride!=batch.indices.Stride) {
                    encoder.BindIndexBuffer(batch.indices); ++bindingChanges;
                }
                if(_indirect) {
                    uint32_t offset=0;
                    while(offset<batch.count) {
                        const auto chunk=TerrainIndirectChunk(batch.count-offset,_nativeLimit);
                        if(!chunk) return false;
                        encoder.DrawIndexedIndirect(slot.indirect->Get(),uint64_t(batch.first+offset)*sizeof(DrawIndexedIndirectArguments),chunk);
                        ++indirectCalls; offset+=chunk;
                    }
                } else {
                    for(const auto& argument:std::span{lists.arguments}.subspan(batch.first,batch.count)) {
                        encoder.DrawIndexed(argument.IndexCount,argument.InstanceCount,argument.FirstIndex,argument.VertexOffset,argument.FirstInstance);
                        ++directCalls;
                    }
                }
                previous=&batch;
            }
            RADRAY_PROFILE_PLOT("Raynium.Submit.DirectCalls",double(directCalls));
            RADRAY_PROFILE_PLOT("Raynium.Submit.IndirectCalls",double(indirectCalls));
            RADRAY_PROFILE_PLOT("Raynium.Submit.BindingChanges",double(bindingChanges));
            RADRAY_PROFILE_PLOT("Raynium.Submit.SelectedGroups",double(draws.selectedGroups));
            RADRAY_PROFILE_PLOT("Raynium.Submit.IndicesInstances",double(draws.indicesInstances));
            auto& stats=*_frames[index];
            stats.directCalls+=directCalls; stats.indirectCalls+=indirectCalls; stats.bindingChanges+=bindingChanges;
            stats.submittedIndices+=draws.indicesInstances; stats.submittedRecords+=draws.recordCount; stats.submittedBatches+=draws.batchCount;
            const bool result=encoder.IsValid();
            if(timing) encoder.GetCommandBuffer()->WriteTimestamp({slot.timestamps.get(),QueryPipelineStage::Bottom,3+passId*2});
            return result;
        });
    };
    geometry(Solid,color,{},true,true,{},-1);
    geometry(Cutout,color,{},false,true,{},-1);
    if(!input.hasTranslucent) return;
    if(!(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) {
        geometry(Translucent,color,{},false,true,{},-1); return;
    }
    const auto use=TextureUse::RenderTarget|TextureUse::Resource|TextureUse::CopySource|TextureUse::CopyDestination;
    const auto bounds=makeTexture(TextureFormat::RGBA32_FLOAT,use),culled=makeTexture(TextureFormat::RGBA32_FLOAT,use);
    const auto coeff0=makeTexture(TextureFormat::RGBA16_FLOAT,use),coeff1=makeTexture(TextureFormat::RGBA16_FLOAT,use);
    const auto accumulate=makeTexture(TextureFormat::RGBA16_FLOAT,use);
    geometry(OitBounds,bounds,{},true,false,{},-1);
    graph.AddCopyPass("Raynium CopyDepthBounds",[&](radray::RDGCopyPassBuilder& pass) {
        pass.ReadTexture(bounds).WriteTexture(culled);
    },[bounds,culled,width,height](radray::RDGCopyPassContext& context) {
        context.GetCommandBuffer().CopyTextureToTexture({.Destination=&context.Resolve(culled),.Source=&context.Resolve(bounds),.Width=width,.Height=height}); return true;
    });
    const auto screen=[&](uint32_t passId,Handle destination,Handles reads,int bindingIndex) {
        _frames[index]->timingMask|=1u<<passId;
        graph.AddRasterPass(TerrainPrograms[passId],[&](radray::RDGRasterPassBuilder& pass) {
            pass.ReadBuffer(constants,BufferState::CBuffer).WriteColor(0,destination,LoadAction::Load,StoreAction::Store)
                .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Load,StoreAction::Store);
            for(auto texture:reads) if(texture.IsValid()) pass.ReadTexture(texture);
        },[this,index,passId,reads,bindingIndex,width,height](radray::RDGRasterPassContext& context) {
            auto& encoder=context.GetEncoder(); auto& slot=_slots[index]; auto* parameters=slot.oitParameters[bindingIndex].get();
            for(uint32_t i=0;i<4;++i) if(!parameters->Set(i,0,context.ResolveView(reads[i].IsValid()?reads[i]:reads[0],TextureViewUsage::Resource).Get())) return false;
            if(!parameters->FlushWrites()) return false;
            const bool timing=(_frames[index]->input.parameters.flags&RN_FRAME_PROFILE) && slot.timestamps;
            if(timing) encoder.GetCommandBuffer()->WriteTimestamp({slot.timestamps.get(),QueryPipelineStage::Top,2+passId*2});
            encoder.SetViewport({0,0,float(width),float(height),0,1}); encoder.SetScissor({0,0,width,height});
            encoder.BindGraphicsPipelineState(_resources->Pipeline(passId));
            if(passId!=OitComposite) encoder.BindShaderParameterSet(0,slot.passParameterSet.get());
            encoder.BindShaderParameterSet(2,parameters);
            encoder.Draw(3,1,0,0);
            if(timing) encoder.GetCommandBuffer()->WriteTimestamp({slot.timestamps.get(),QueryPipelineStage::Bottom,3+passId*2});
            return true;
        });
    };
    screen(OitCull,culled,{bounds},0);
    geometry(OitTransmittance,coeff0,coeff1,true,false,{culled},1);
    geometry(OitAccumulate,accumulate,{},true,false,{culled,coeff0,coeff1},2);
    screen(OitComposite,color,{bounds,coeff0,coeff1,accumulate},3);
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
        before->ResetQueryPool(resources.timestamps.get(),0,18);
        before->WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Top,0});
        before->End();
        after->Begin();
        BufferBarrier(*after,resources.timing.get(),BufferState::Undefined,BufferState::CopyDestination);
        after->WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Bottom,1});
        after->ResolveQueryData({resources.timestamps.get(),0,2,resources.timing.get(),0});
        for(uint32_t pass=0;pass<8;++pass) if(frame.timingMask&(1u<<pass))
            after->ResolveQueryData({resources.timestamps.get(),2+pass*2,2,resources.timing.get(),16+uint64_t(pass)*16});
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
    RADRAY_PROFILE_PLOT("Raynium.Frame.DirectCalls",double(frame.directCalls));
    RADRAY_PROFILE_PLOT("Raynium.Frame.IndirectCalls",double(frame.indirectCalls));
    RADRAY_PROFILE_PLOT("Raynium.Frame.BindingChanges",double(frame.bindingChanges));
    RADRAY_PROFILE_PLOT("Raynium.Frame.SubmittedIndicesInstances",double(frame.submittedIndices));
    RADRAY_PROFILE_PLOT("Raynium.Frame.SubmittedRecords",double(frame.submittedRecords));
    RADRAY_PROFILE_PLOT("Raynium.Frame.SubmittedBatches",double(frame.submittedBatches));
    frame.input={};
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
    if((frame->flags&RN_FRAME_PROFILE) && resources.timestamps && resources.timing) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.ReadGpuTiming");
        auto* ticks=static_cast<uint64_t*>(resources.timing->Map(0,144));
        if(ticks) {
            resources.timing->InvalidateMappedRange({0,144});
            const double sample=double(ticks[1]-ticks[0])*resources.timestamps->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs/1e6;
            const auto period=resources.timestamps->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs/1e6;
            for(uint32_t pass=0;pass<8;++pass) if(frame->timingMask&(1u<<pass)) {
                auto& samples=_stageSamples[pass]; auto& count=_stageCounts[pass];
                samples[count++]=double(ticks[3+pass*2]-ticks[2+pass*2])*period;
                if(count==samples.size()) {
                    std::sort(samples.begin(),samples.end());
                    RADRAY_INFO_LOG("Terrain GPU stage={} samples=120 ms_p50={} p95={} p99={} visible_commands={}",TerrainPrograms[pass],samples[60],samples[114],samples[118],pass<6?frame->visibleCommands[pass]:1); count=0;
                }
            }
            _gpuSamples[_gpuSampleCount++]=sample;
            RADRAY_PROFILE_PLOT("Raynium.GpuFrame.Sequence",double(frame->sequence));
            RADRAY_PROFILE_PLOT("Raynium.GpuMilliseconds", sample);
            resources.timing->Unmap();
            if(_gpuSampleCount==_gpuSamples.size()) {
                std::sort(_gpuSamples.begin(),_gpuSamples.end());
                RADRAY_INFO_LOG("Raynium terrain GPU samples=120 ms_p50={} p95={} p99={}",_gpuSamples[60],_gpuSamples[114],_gpuSamples[118]);
                _gpuSampleCount=0;
            }
        } else RADRAY_WARN_LOG("Cannot read completed terrain timestamps");
    }
    _frames[slot.GetIndex()]=nullptr;
}
}
