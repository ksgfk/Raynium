#include <raynium/solid_pipeline.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/frame_upload.h>
#include <raynium/frame_record.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::render;
bool SolidFramePipeline::Create(radray::DeviceContext& device,std::span<const radray::byte> shaders,bool indirect,radray::unique_ptr<SolidFramePipeline>& out) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Pipeline.Create");
    if(out) RADRAY_ABORT("Frame pipeline output occupied");
    auto& d=device.GetDevice();
    radray::unique_ptr<MeshPassResources> resources;
    if(!MeshPassResources::Create(d,shaders,resources)) return false;
    radray::unique_ptr<GuiPassResources> gui;
    if(!GuiPassResources::Create(d,shaders,gui)) return false;
    auto completion=d.CreateFence().Release();
    if(!completion) { RADRAY_ERR_LOG("Cannot create terrain completion fence"); return false; }
    const bool timestamps=d.GetCapabilities().Features.TimestampQueries;
    radray::array<SlotResources,2> slots;
    for(auto& slot:slots) {
        if(!gui->CreateSlot(d,slot.gui)) return false;
        slot.passParameterSet=d.CreateShaderParameterSet({resources->PassGroupLayout()}).Release();
        if(!slot.passParameterSet) { RADRAY_ERR_LOG("Cannot create frame slot pass bindings"); return false; }
        for(auto& parameters:slot.oitParameters) {
            parameters=d.CreateShaderParameterSet({resources->OitGroupLayout()}).Release();
            if(!parameters) { RADRAY_ERR_LOG("Cannot create OIT slot bindings"); return false; }
        }
        if(timestamps) {
            slot.timestamps=d.CreateQueryPool({QueryType::Timestamp,18,"Raynium terrain frame"}).Release();
            slot.timing=CreateBuffer(d,144,MemoryType::ReadBack,BufferUse::CopyDestination|BufferUse::MapRead);
            if(!slot.timestamps || !slot.timing) { RADRAY_ERR_LOG("Cannot create terrain timing resources"); return false; }
        }
    }
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(device,std::move(resources),std::move(gui),std::move(slots),std::move(completion),indirect));
    return true;
}
SlotUploads SolidFramePipeline::GetUploads(radray::FrameSlot& frame) noexcept try {
    if(frame.GetIndex()>=_slots.size()) RADRAY_ABORT("Invalid frame upload slot: {}",frame.GetIndex());
    auto& slot=_slots[frame.GetIndex()];
    slot.geometry.clear();
    slot.gui.vertices={};
    return SlotUploads{frame,slot.geometry,slot.lists,_indirect};
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::GetUploads");
}
void SolidFramePipeline::Finish() {
    if(_submitted && !_completion->WaitFor(_submitted,5000))
        RADRAY_ABORT("Renderer completion timed out: value={} timeout_ms=5000; resources cannot be safely reclaimed",_submitted);
}
void SolidFramePipeline::ExtractFrameData(radray::FrameSlot& slot) noexcept try {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.Extract");
    if(!_pending || slot.GetIndex()>=_frames.size()) RADRAY_ABORT("Missing terrain frame extraction source");
    auto* frame=slot.Emplace<FrameData>(*_pending);
    _frames[slot.GetIndex()]=frame;
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::ExtractFrameData");
}
void SolidFramePipeline::SetupGraph(radray::RDGBuilder& graph,radray::FrameSlot& slot) noexcept try {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SetupGraph");
    const auto index=slot.GetIndex();
    if(index>=_frames.size() || !_frames[index]) RADRAY_ABORT("Missing terrain graph frame: slot={}",index);
    auto& resources=_slots[index]; auto& frame=*_frames[index];
    const auto& input=frame.input; const auto& f=input.parameters;
    for(auto& upload:*input.geometry) {
        const auto target=graph.ImportPersistentOwned(upload.target);
        graph.UploadBuffer(target,upload.offset,std::move(upload.source)); graph.MarkOutput(target);
    }
    input.geometry->clear();
    input.textures->AddUploads(graph);
    if(!input.color) return;
    const auto color=graph.ImportTexture(input.color,TextureState::Undefined,TextureState::Present);
    const auto output=input.color->GetDesc();
    const uint32_t width=output.Width,height=output.Height;
    graph.MarkOutput(color);
    struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4]; } values{};
    static_assert(sizeof(Constants)==192);
    std::copy_n(f.projection,16,values.projection); std::copy_n(f.view,16,values.view); std::copy_n(f.camera_block,3,values.block);
    values.block[3]=(f.flags&RN_FRAME_RGSS)!=0; std::copy_n(f.camera_fraction,3,values.fraction);
    values.fraction[3]=slot.GetDevice().GetBackend()==RenderBackend::Vulkan?1.f:-1.f;
    if(f.flags&RN_FRAME_FLIP_Y) values.fraction[3]*=-1;
    std::copy_n(f.fog_color,4,values.fog); std::copy_n(f.fog_distances,4,values.distances);
    const auto constants=graph.CreateBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,values,
        +[](const Constants& values,radray::RDGUploadWriteContext& context) noexcept {
            std::memset(context.Bytes.data(),0,context.Bytes.size()); std::memcpy(context.Bytes.data(),&values,sizeof(values)); return true;
        }));
    const auto instanceBytes=std::as_bytes(input.instances);
    const auto instances=graph.CreateBuffer({.MaxSize=instanceBytes.size(),.Usage=BufferUse::Vertex},radray::RDGUploadSource::BorrowedBytes(instanceBytes));
    const auto arguments=std::as_bytes(std::span{input.lists->arguments});
    const auto indirect=_indirect?graph.CreateBuffer({.MaxSize=arguments.size(),.Usage=BufferUse::Indirect},radray::RDGUploadSource::BorrowedBytes(arguments)):radray::RDGBufferHandle{};
    const auto makeTexture=[&](TextureFormat format,TextureUses usage) {
        return graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,
            .MipLevels=1,.SampleCount=1,.Format=format,.Memory=MemoryType::Device,.Usage=usage});
    };
    const auto depth=makeTexture(TextureFormat::D32_FLOAT,TextureUse::DepthStencilWrite|TextureUse::DepthStencilRead);
    struct LayerAccess {
        explicit LayerAccess(std::pmr::memory_resource& memory):vertices(&memory),indices(&memory),textures(&memory) {}
        std::pmr::vector<radray::RDGBufferHandle> vertices,indices;
        std::pmr::vector<radray::RDGTextureHandle> textures;
    };
    radray::array<LayerAccess,3> accesses{LayerAccess{slot.GetScratchMemoryResource()},LayerAccess{slot.GetScratchMemoryResource()},LayerAccess{slot.GetScratchMemoryResource()}};
    for(uint32_t layer=0;layer<3;++layer) {
        auto& access=accesses[layer]; const auto& draws=input.lists->layers[layer];
        std::pmr::unordered_set<radray::GpuBuffer*> vertexPages{&slot.GetScratchMemoryResource()},indexPages{&slot.GetScratchMemoryResource()};
        std::pmr::unordered_set<radray::GpuTexture*> textures{&slot.GetScratchMemoryResource()};
        for(const auto& batch:std::span{input.lists->batches}.subspan(draws.firstBatch,draws.batchCount)) {
            if(!batch.vertexPage || !batch.indexPage) RADRAY_ABORT("Missing terrain physical page");
            if(vertexPages.insert(batch.vertexPage).second) access.vertices.push_back(graph.ImportPersistentBorrowed(*batch.vertexPage));
            if(indexPages.insert(batch.indexPage).second) access.indices.push_back(graph.ImportPersistentBorrowed(*batch.indexPage));
            for(const auto& texture:batch.material->textures) if(texture && textures.insert(texture.Get()).second)
                access.textures.push_back(graph.ImportPersistentOwned(texture));
        }
    }
    struct PassData {
        SlotResources* resources;
        GraphicsPipelineState* pipeline;
        const TerrainFrameLists* lists;
        uint32_t passId,width,height,nativeLimit;
        int bindingIndex;
        bool useIndirect,timing,screen;
        radray::RDGBufferHandle constants,instances,indirect;
        radray::array<radray::RDGTextureHandle,4> reads;
        radray::RDGBufferSlice instanceSlice{},indirectSlice{};
    };
    const auto prepare=+[](PassData& data,radray::RDGPrepareContext& context) noexcept {
        try {
        if(!BindFrameConstants(*data.resources->passParameterSet,data.resources->boundConstants,context.ResolveBuffer(data.constants))) return false;
        if(!data.screen) {
            data.instanceSlice=context.ResolveBuffer(data.instances);
            if(data.useIndirect) data.indirectSlice=context.ResolveBuffer(data.indirect);
        }
        if(data.bindingIndex>=0) {
            auto* parameters=data.resources->oitParameters[data.bindingIndex].get();
            // Registered after CompiledRDG: command reset precedes unbinding, which precedes view cleanup.
            struct BindingCleanup {
                ShaderParameterSet* parameters;
                ~BindingCleanup() noexcept { parameters->ResetBindingsForReuse(); }
            };
            context.GetSlot().Emplace<BindingCleanup>(parameters);
            for(uint32_t i=0;i<4;++i) {
                const auto view=context.ResolveView(data.reads[i].IsValid()?data.reads[i]:data.reads[0],TextureViewUsage::Resource);
                if(!view || !parameters->Set(i,0,view.Get())) return false;
            }
            if(!parameters->FlushWrites()) return false;
        }
        return true;
        } catch(...) { AbortUnexpectedException("Raynium Terrain prepare"); }
    };
    const auto record=+[](const PassData& data,radray::RDGRasterPassContext& context) noexcept {
        try {
            RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Record");
            auto& encoder=context.GetEncoder(); const auto& resources=*data.resources;
            if(data.timing) encoder.GetCommandBuffer()->WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Top,2+data.passId*2});
            encoder.SetViewport({0,0,float(data.width),float(data.height),0,1}); encoder.SetScissor({0,0,data.width,data.height});
            encoder.BindGraphicsPipelineState(data.pipeline);
            if(data.passId!=OitComposite) encoder.BindShaderParameterSet(0,resources.passParameterSet.get());
            if(data.bindingIndex>=0) encoder.BindShaderParameterSet(2,resources.oitParameters[data.bindingIndex].get());
            if(data.screen) encoder.Draw(3,1,0,0);
            else if(!RecordTerrainDraws({data.lists,data.passId<=Translucent?data.passId:uint32_t(Translucent),data.nativeLimit,
                data.useIndirect,data.instanceSlice,data.indirectSlice},encoder)) return false;
            if(data.timing) encoder.GetCommandBuffer()->WriteTimestamp({resources.timestamps.get(),QueryPipelineStage::Bottom,3+data.passId*2});
            return encoder.IsValid();
        } catch(...) { AbortUnexpectedException("Raynium Terrain record"); }
    };
    const bool timing=(f.flags&RN_FRAME_PROFILE) && resources.timestamps;
    const auto geometry=[&](uint32_t passId,radray::RDGTextureHandle destination,radray::RDGTextureHandle second,bool clear,bool writeDepth,
        radray::array<radray::RDGTextureHandle,4> reads,int bindingIndex) {
        frame.timingMask|=1u<<passId;
        const auto layer=passId<=Translucent?passId:uint32_t(Translucent); const auto& draws=input.lists->layers[layer];
        frame.visibleCommands[passId]=draws.recordCount;
        frame.submittedIndices+=draws.indicesInstances; frame.submittedRecords+=draws.recordCount; frame.submittedBatches+=draws.batchCount;
        frame.bindingChanges+=2+(bindingIndex>=0?1:0)+(!input.instances.empty()?1:0);
        const TerrainDrawBatch* previous=nullptr;
        for(const auto& batch:std::span{input.lists->batches}.subspan(draws.firstBatch,draws.batchCount)) {
            frame.bindingChanges+=!previous || previous->material->parameters.get()!=batch.material->parameters.get();
            frame.bindingChanges+=!previous || previous->vertices.Target!=batch.vertices.Target || previous->vertices.Offset!=batch.vertices.Offset || previous->vertices.Size!=batch.vertices.Size;
            frame.bindingChanges+=!previous || previous->indices.Target!=batch.indices.Target || previous->indices.Offset!=batch.indices.Offset || previous->indices.Stride!=batch.indices.Stride;
            if(_indirect) frame.indirectCalls+=(uint64_t(batch.count)+_nativeLimit-1)/_nativeLimit; else frame.directCalls+=batch.count;
            previous=&batch;
        }
        auto pass=graph.AddRasterPass(TerrainPrograms[passId],PassData{&resources,_resources->Pipeline(passId,output.Format,(f.flags&RN_FRAME_FLIP_Y)!=0),
            input.lists,passId,width,height,_nativeLimit,bindingIndex,_indirect,timing,false,constants,instances,indirect,reads},record,prepare);
        pass.ReadBuffer(constants,BufferState::CBuffer).ReadBuffer(instances,BufferState::Vertex);
        if(_indirect) pass.ReadBuffer(indirect,BufferState::Indirect);
        pass.WriteColor(0,destination,clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,
            passId==Solid?ColorClearValue{{f.clear_color[0],f.clear_color[1],f.clear_color[2],f.clear_color[3]}}:
            passId==OitBounds?ColorClearValue{{-std::numeric_limits<float>::max(),0,0,0}}:ColorClearValue{});
        if(second.IsValid()) pass.WriteColor(1,second,LoadAction::Clear,StoreAction::Store);
        pass.SetDepthStencil(depth,writeDepth?TextureState::DepthWrite:TextureState::DepthRead,passId==Solid?LoadAction::Clear:LoadAction::Load,
            StoreAction::Store,LoadAction::DontCare,StoreAction::Discard,{0,0});
        for(auto handle:accesses[layer].vertices) pass.ReadBuffer(handle,BufferState::Vertex);
        for(auto handle:accesses[layer].indices) pass.ReadBuffer(handle,BufferState::Index);
        for(auto handle:accesses[layer].textures) pass.ReadTexture(handle);
        for(auto handle:reads) if(handle.IsValid()) pass.ReadTexture(handle);
    };
    const auto gui=[&] { _gui->AddPass(graph,slot,resources.gui,color,width,height); };
    geometry(Solid,color,{},true,true,{},-1); geometry(Cutout,color,{},false,true,{},-1);
    if(!input.hasTranslucent) { gui(); return; }
    if(!(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) { geometry(Translucent,color,{},false,true,{},-1); gui(); return; }
    const auto use=TextureUse::RenderTarget|TextureUse::Resource|TextureUse::CopySource|TextureUse::CopyDestination;
    const auto bounds=makeTexture(TextureFormat::RGBA32_FLOAT,use),culled=makeTexture(TextureFormat::RGBA32_FLOAT,use);
    const auto coeff0=makeTexture(TextureFormat::RGBA16_FLOAT,use),coeff1=makeTexture(TextureFormat::RGBA16_FLOAT,use),accumulate=makeTexture(TextureFormat::RGBA16_FLOAT,use);
    geometry(OitBounds,bounds,{},true,false,{},-1);
    struct CopyData { radray::RDGTextureHandle source,destination; uint32_t width,height; };
    graph.AddCopyPass("Raynium CopyDepthBounds",CopyData{bounds,culled,width,height},+[](const CopyData& data,radray::RDGCopyPassContext& context) noexcept {
        context.GetCommandBuffer().CopyTextureToTexture({.Destination=&context.Resolve(data.destination),.Source=&context.Resolve(data.source),.Width=data.width,.Height=data.height}); return true;
    }).ReadTexture(bounds).WriteTexture(culled);
    const auto screen=[&](uint32_t passId,radray::RDGTextureHandle destination,radray::array<radray::RDGTextureHandle,4> reads,int bindingIndex) {
        frame.timingMask|=1u<<passId;
        auto pass=graph.AddRasterPass(TerrainPrograms[passId],PassData{&resources,_resources->Pipeline(passId,output.Format),input.lists,
            passId,width,height,_nativeLimit,bindingIndex,false,timing,true,constants,{},{},reads},record,prepare);
        pass.ReadBuffer(constants,BufferState::CBuffer).WriteColor(0,destination,LoadAction::Load,StoreAction::Store)
            .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Load,StoreAction::Store);
        for(auto handle:reads) if(handle.IsValid()) pass.ReadTexture(handle);
    };
    screen(OitCull,culled,{bounds},0); geometry(OitTransmittance,coeff0,coeff1,true,false,{culled},1);
    geometry(OitAccumulate,accumulate,{},true,false,{culled,coeff0,coeff1},2); screen(OitComposite,color,{bounds,coeff0,coeff1,accumulate},3); gui();
} catch(...) { AbortUnexpectedException("SolidFramePipeline::SetupGraph"); }

void SolidFramePipeline::SubmitFrame(radray::FrameSlot& slot,CommandBuffer& graphCommands) noexcept try {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SubmitHandoff");
    if(_submitted==std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Terrain completion sequence exhausted");
    if(slot.GetIndex()>=_frames.size() || !_frames[slot.GetIndex()]) RADRAY_ABORT("Missing terrain submit frame: slot={}",slot.GetIndex());
    auto& frame=*_frames[slot.GetIndex()];
    auto& input=frame.input; const auto& f=input.parameters;
    auto& resources=_slots[slot.GetIndex()];
    const bool timing=(f.flags&RN_FRAME_PROFILE)!=0 && resources.timestamps && resources.timing;
    radray::array<CommandBuffer*,3> commands{&graphCommands}; size_t commandCount=1;
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
    radray::array<Fence*,1> signals{_completion.get()};
    radray::array<uint64_t,1> signalValues{_submitted+1};
    SwapChainSyncObject* acquire[]{input.waitToDraw}; SwapChainSyncObject* present[]{input.readyToPresent};
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.QueueSubmit");
        slot.GetQueue().Submit({.CmdBuffers={commands.data(),commandCount},.SignalFences=signals,.SignalValues=signalValues,
            .WaitToExecute={acquire,input.waitToDraw?1u:0u},.ReadyToPresent={present,input.readyToPresent?1u:0u}});
    }
    frame.completion=++_submitted;
    RADRAY_PROFILE_PLOT("Raynium.Frame.DirectCalls",double(frame.directCalls));
    RADRAY_PROFILE_PLOT("Raynium.Frame.IndirectCalls",double(frame.indirectCalls));
    RADRAY_PROFILE_PLOT("Raynium.Frame.BindingChanges",double(frame.bindingChanges));
    RADRAY_PROFILE_PLOT("Raynium.Frame.SubmittedIndicesInstances",double(frame.submittedIndices));
    RADRAY_PROFILE_PLOT("Raynium.Frame.SubmittedRecords",double(frame.submittedRecords));
    RADRAY_PROFILE_PLOT("Raynium.Frame.SubmittedBatches",double(frame.submittedBatches));
    resources.gui.vertices={};
    frame.input={};
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::SubmitFrame");
}
void SolidFramePipeline::Wait(radray::FrameSlot& slot) noexcept try {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.WaitCompleted");
    if(slot.GetIndex()>=_frames.size()) RADRAY_ABORT("Invalid terrain completion slot: {}",slot.GetIndex());
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
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::Wait");
}
}
