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
    radray::unique_ptr<OutputPassResources> output;
    if(!OutputPassResources::Create(d,shaders,output)) return false;
    radray::unique_ptr<ScenePassResources> scene;
    if(!ScenePassResources::Create(shaders,scene)) return false;
    auto completion=d.CreateFence().Release();
    if(!completion) { RADRAY_ERR_LOG("Cannot create terrain completion fence"); return false; }
    const bool timestamps=d.GetCapabilities().Features.TimestampQueries;
    radray::array<SlotResources,2> slots;
    for(auto& slot:slots) {
        if(!gui->CreateSlot(d,slot.gui)) return false;
        slot.output.parameters=output->CreateParameters(d);
        if(!slot.output.parameters) return false;
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
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(device,std::move(resources),std::move(gui),std::move(output),std::move(scene),std::move(slots),std::move(completion),indirect));
    return true;
}
SlotUploads SolidFramePipeline::GetUploads(radray::FrameSlot& frame) noexcept try {
    if(frame.GetIndex()>=_slots.size()) RADRAY_ABORT("Invalid frame upload slot: {}",frame.GetIndex());
    auto& slot=_slots[frame.GetIndex()];
    slot.geometry.clear();
    slot.textureImports.clear();
    slot.captures.clear();
    slot.gui.vertices={};
    return SlotUploads{frame,slot.geometry,slot.lists,_indirect};
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::GetUploads");
}
void SolidFramePipeline::Finish() {
    if(_submitted && !_completion->WaitFor(_submitted,5000))
        RADRAY_ABORT("Renderer completion timed out: value={} timeout_ms=5000; resources cannot be safely reclaimed",_submitted);
}
bool SolidFramePipeline::PrepareCapture(radray::FrameSlot& slot,const rn_render_frame_desc& frame,uint32_t width,uint32_t height) {
    auto& resources=_slots[slot.GetIndex()]; auto& device=slot.GetDevice();
    if(!resources.color || resources.color->GetTexture()->GetDesc().Width!=width || resources.color->GetTexture()->GetDesc().Height!=height) {
        auto replacement=TextureResource::Create(device,{sizeof(rn_uploaded_texture_desc),0,width,height,1,RN_PIXEL_RGBA8,1,RN_TEXTURE_ATTACHMENT|RN_TEXTURE_READBACK});
        if(!replacement) { RADRAY_ERR_LOG("Cannot create scene color target"); return false; }
        resources.output.parameters->ResetBindingsForReuse(); resources.output.texture.Reset();
        resources.color=std::move(replacement);
    }
    if(frame.flags&RN_FRAME_CAPTURE) {
        const rn_capture_request request{sizeof(rn_capture_request),(frame.flags&RN_FRAME_CAPTURE_PREVIOUS)?1u:0u,frame.output_sequence};
        if(!RequestCapture({},request)) return false;
    }
    for(const auto& queued:_captureRequests) {
        const auto& request=queued.request;
        auto source=queued.texture?queued.texture:resources.color;
        const auto& desc=source->GetTexture()->GetDesc();
        auto captureWidth=std::max(1u,desc.Width>>request.mip),captureHeight=std::max(1u,desc.Height>>request.mip);
        if(request.source==4) {
            const float aspect=float(captureWidth)/float(captureHeight);
            if(captureWidth>320) { captureWidth=320; captureHeight=uint32_t(320.f/aspect); }
            if(captureHeight>180) { captureWidth=uint32_t(180.f*aspect); captureHeight=180; }
            captureWidth=std::max(4u,captureWidth/4*4); captureHeight=std::max(4u,captureHeight/4*4);
            if(!_scene->PrepareTracy(device,resources.scene)) return false;
        }
        const uint64_t alignment=std::max(uint64_t{1},uint64_t(device.GetCapabilities().Detail.TextureDataPitchAlignment));
        const uint64_t pitch=(uint64_t(captureWidth)*4+alignment-1)/alignment*alignment;
        auto buffer=CreateBuffer(device,pitch*captureHeight,MemoryType::ReadBack,BufferUse::CopyDestination|BufferUse::MapRead);
        if(!buffer) { RADRAY_ERR_LOG("Cannot create capture readback buffer"); return false; }
        auto capture=radray::make_shared<Capture>(std::move(buffer),std::move(source),captureWidth,captureHeight,pitch,request);
        resources.captures.push_back(capture); _captures.emplace(request.id,std::move(capture));
    }
    _captureRequests.clear(); return true;
}
bool SolidFramePipeline::RequestCapture(const UploadedTextures& textures,const rn_capture_request& request) {
    const auto fail=[] { RADRAY_ERR_LOG("Invalid capture request, duplicate ID, unavailable source or unread capture limit exceeded"); return false; };
    if(request.struct_size!=sizeof(request) || request.reserved || request.source>4 || !request.id || _captures.contains(request.id) ||
        _captures.size()+_captureRequests.size()>=4096 || std::any_of(_captureRequests.begin(),_captureRequests.end(),
            [&](const auto& queued) { return queued.request.id==request.id; })) return fail();
    radray::IntrusivePtr<TextureResource> texture;
    if(request.source==3) {
        const auto* found=textures.Find(request.texture);
        if(!found || found->desc.format!=RN_PIXEL_RGBA8 || request.mip>=found->desc.mip_levels || request.layer>=found->desc.array_layers ||
            !(found->texture->GetTexture()->GetDesc().Usage&TextureUse::CopySource)) return fail();
        texture=found->texture;
    } else {
        if(request.texture || request.mip || request.layer) return fail();
        if(request.source==1) { texture=_lastColor; if(!texture) return fail(); }
    }
    _captureRequests.push_back({request,std::move(texture)}); return true;
}

bool SolidFramePipeline::PollCapture(uint64_t sequence,rn_capture_info& info,void* pixels,uint64_t capacity) {
    auto found=_captures.find(sequence);
    if(found==_captures.end()) {
        if(std::any_of(_captureRequests.begin(),_captureRequests.end(),[&](const auto& queued){return queued.request.id==sequence;})) {
            info={sizeof(info)}; return true;
        }
        RADRAY_ERR_LOG("Unknown or consumed capture sequence: {}",sequence); return false;
    }
    auto& capture=*found->second;
    const uint64_t size=uint64_t(capture.width)*capture.height*4;
    if(pixels && capacity<size) { RADRAY_ERR_LOG("Capture destination is too small: capacity={} required={}",capacity,size); return false; }
    const bool ready=capture.completion && _completion->GetCompletedValue()>=capture.completion;
    info={sizeof(info),ready?1u:0u,capture.width,capture.height,size,capture.width*4,0};
    if(!ready || !pixels) return true;
    const auto* bytes=static_cast<const radray::byte*>(capture.buffer->Map(0,capture.pitch*capture.height));
    if(!bytes) { RADRAY_ERR_LOG("Cannot map completed capture: sequence={}",sequence); return false; }
    capture.buffer->InvalidateMappedRange({0,capture.pitch*capture.height});
    for(uint32_t row=0;row<capture.height;++row)
        std::memcpy(static_cast<radray::byte*>(pixels)+uint64_t(row)*capture.width*4,bytes+uint64_t(row)*capture.pitch,uint64_t(capture.width)*4);
    capture.buffer->Unmap(); _captures.erase(found); return true;
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
        if(resources.scene.metrics) resources.metrics.terrain_upload_bytes+=upload.source.GetMaxSize();
        const auto target=graph.ImportPersistentOwned(upload.target);
        graph.UploadBuffer(target,upload.offset,std::move(upload.source)); graph.MarkOutput(target);
    }
    input.geometry->clear();
    input.textures->AddUploads(graph,slot,resources.textureImports,resources.scene.metrics);
    _scene->AddBufferUploads(graph,resources.scene.metrics);
    _scene->Upload(graph,slot,resources.scene);
    _scene->AddOffscreenPasses(graph,slot,resources.scene,resources.textureImports);
    if(!input.color && resources.captures.empty() && !f.render_width) return;
    const auto destination=input.color?graph.ImportTexture(input.color,TextureState::Undefined,TextureState::Present):radray::RDGTextureHandle{};
    const uint32_t width=input.width,height=input.height;
    const auto color=resources.color->Import(graph,slot,resources.textureImports);
    graph.MarkOutput(color);
    if(destination.IsValid()) graph.MarkOutput(destination);
    const auto capture=[&](uint32_t stage) {
        radray::RDGTextureHandle tracy;
        for(const auto& request:resources.captures) {
            if(request->request.source!=stage) continue;
            slot.Emplace<radray::shared_ptr<Capture>>(request);
            const auto buffer=graph.ImportBuffer(request->buffer.get(),BufferState::Undefined,BufferStates{BufferState::HostRead});
            struct Readback {
                radray::RDGTextureHandle source; radray::RDGBufferHandle destination;
                radray::RDGBufferSlice slice{}; Texture* texture{}; uint32_t mip,layer;
            };
            auto source=request->source->Import(graph,slot,resources.textureImports);
            if(stage==4) {
                if(!tracy.IsValid()) tracy=_scene->AddTracy(graph,slot,resources.scene,source,request->width,request->height);
                source=tracy;
            }
            graph.AddCopyPass("Raynium Capture",Readback{source,buffer,{},nullptr,request->request.mip,request->request.layer},+[](const Readback& data,radray::RDGCopyPassContext& context) noexcept {
                context.GetCommandBuffer().CopyTextureToBuffer(data.slice.Buffer.Get(),data.slice.Offset,data.texture,{data.layer,1,data.mip,1,TextureAspect::Color});
                return true;
            },+[](Readback& data,radray::RDGPrepareContext& context) noexcept {
                data.slice=context.ResolveBuffer(data.destination); data.texture=&context.Resolve(data.source);
                return data.slice.Buffer.HasValue();
            }).ReadTexture(source).WriteBuffer(buffer).NeverCull();
        }
    };
    capture(1); capture(3);
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
    if(resources.scene.metrics) resources.metrics.terrain_upload_bytes+=256+instanceBytes.size()+(_indirect?arguments.size():0);
    const auto indirect=_indirect?graph.CreateBuffer({.MaxSize=arguments.size(),.Usage=BufferUse::Indirect},radray::RDGUploadSource::BorrowedBytes(arguments)):radray::RDGBufferHandle{};
    const auto makeTexture=[&](TextureFormat format,TextureUses usage) {
        return graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,
            .MipLevels=1,.SampleCount=1,.Format=format,.Memory=MemoryType::Device,.Usage=usage});
    };
    const auto depthUsage=TextureUse::DepthStencilWrite|TextureUse::DepthStencilRead|TextureUse::Resource;
    const auto depth=makeTexture(TextureFormat::D32_FLOAT,depthUsage);
    auto geometryDepth=depth;
    const auto copyTexture=[&](std::string_view name,radray::RDGTextureHandle source,radray::RDGTextureHandle target) {
        struct Data { radray::RDGTextureHandle source,target; Texture* sourceTexture{}; Texture* targetTexture{}; uint32_t width,height; };
        graph.AddCopyPass(name,Data{source,target,nullptr,nullptr,width,height},+[](const Data& data,radray::RDGCopyPassContext& context) noexcept {
            context.GetCommandBuffer().CopyTextureToTexture({.Destination=data.targetTexture,.Source=data.sourceTexture,.Width=data.width,.Height=data.height}); return true;
        },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
            data.sourceTexture=&context.Resolve(data.source); data.targetTexture=&context.Resolve(data.target); return true;
        }).ReadTexture(source).WriteTexture(target);
    };
    struct LayerAccess {
        explicit LayerAccess(std::pmr::memory_resource& memory):vertices(&memory),indices(&memory),textures(&memory) {}
        std::pmr::vector<radray::RDGBufferHandle> vertices,indices;
        std::pmr::vector<radray::RDGTextureHandle> textures;
    };
    radray::array<LayerAccess,3> accesses{LayerAccess{slot.GetScratchMemoryResource()},LayerAccess{slot.GetScratchMemoryResource()},LayerAccess{slot.GetScratchMemoryResource()}};
    for(uint32_t layer=0;layer<3;++layer) {
        auto& access=accesses[layer]; const auto& draws=input.lists->layers[layer];
        std::pmr::unordered_set<radray::GpuBuffer*> vertexPages{&slot.GetScratchMemoryResource()},indexPages{&slot.GetScratchMemoryResource()};
        std::pmr::unordered_set<TextureResource*> textures{&slot.GetScratchMemoryResource()};
        for(const auto& batch:std::span{input.lists->batches}.subspan(draws.firstBatch,draws.batchCount)) {
            if(!batch.vertexPage || !batch.indexPage) RADRAY_ABORT("Missing terrain physical page");
            if(vertexPages.insert(batch.vertexPage).second) access.vertices.push_back(graph.ImportPersistentBorrowed(*batch.vertexPage));
            if(indexPages.insert(batch.indexPage).second) access.indices.push_back(graph.ImportPersistentBorrowed(*batch.indexPage));
            for(const auto& texture:batch.material->textures) if(texture && textures.insert(texture.Get()).second)
                access.textures.push_back(texture->Import(graph,slot,resources.textureImports));
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
        uint32_t* timingMask;
        radray::RDGBufferSlice instanceSlice{},indirectSlice{};
    };
    const auto prepare=+[](PassData& data,radray::RDGPrepareContext& context) noexcept {
        try {
        if(data.timing) *data.timingMask|=1u<<data.passId;
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
        auto pass=graph.AddRasterPass(TerrainPrograms[passId],PassData{&resources,_resources->Pipeline(passId,TextureFormat::RGBA8_UNORM,(f.flags&RN_FRAME_FLIP_Y)!=0,(f.flags&RN_FRAME_WIREFRAME)!=0),
            input.lists,passId,width,height,_nativeLimit,bindingIndex,_indirect,timing,false,constants,instances,indirect,reads,&frame.timingMask},record,prepare);
        pass.ReadBuffer(constants,BufferState::CBuffer).ReadBuffer(instances,BufferState::Vertex);
        if(_indirect) pass.ReadBuffer(indirect,BufferState::Indirect);
        pass.WriteColor(0,destination,clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,
            passId==Solid?ColorClearValue{{f.clear_color[0],f.clear_color[1],f.clear_color[2],f.clear_color[3]}}:
            passId==OitBounds?ColorClearValue{{-std::numeric_limits<float>::max(),0,0,0}}:ColorClearValue{});
        if(second.IsValid()) pass.WriteColor(1,second,LoadAction::Clear,StoreAction::Store);
        pass.SetDepthStencil(geometryDepth,writeDepth?TextureState::DepthWrite:TextureState::DepthRead,passId==Solid && !resources.scene.phaseCounts[RN_SCENE_SKY]?LoadAction::Clear:LoadAction::Load,
            StoreAction::Store,LoadAction::DontCare,StoreAction::Discard,{0,0});
        for(auto handle:accesses[layer].vertices) pass.ReadBuffer(handle,BufferState::Vertex);
        for(auto handle:accesses[layer].indices) pass.ReadBuffer(handle,BufferState::Index);
        for(auto handle:accesses[layer].textures) pass.ReadTexture(handle);
        for(auto handle:reads) if(handle.IsValid()) pass.ReadTexture(handle);
    };
    const auto scene=[&](rn_scene_phase phase) { _scene->AddPass(graph,slot,resources.scene,phase,color,depth,width,height,resources.textureImports); };
    const auto gui=[&] {
        scene(RN_SCENE_AFTER_TERRAIN); scene(RN_SCENE_SEE_THROUGH);
        const auto overlay=[&](rn_scene_phase phase,int index) {
            if(!resources.scene.phaseCounts[phase]) return;
            const auto target=resources.scene.consistentDepth?makeTexture(TextureFormat::D32_FLOAT,depthUsage):depth;
            _scene->AddPass(graph,slot,resources.scene,phase,color,target,width,height,resources.textureImports,{},{},true);
            if(resources.scene.consistentDepth) _scene->AddDepthCopy(graph,resources.scene,target,depth,width,height,index);
        };
        overlay(RN_SCENE_ALWAYS_ON_TOP,0); overlay(RN_SCENE_HAND,1);
        capture(2);
        if(f.flags&RN_FRAME_WORLD_ONLY) { capture(0); capture(4); return; }
        _scene->AddOutline(graph,slot,resources.scene,color,depth,width,height,resources.textureImports);
        _scene->AddPostEffects(graph,slot,resources.scene,0,color,depth,width,height);
        scene(RN_SCENE_PANORAMA);
        _gui->Upload(graph,resources.gui);
        const bool blur=std::any_of(resources.scene.effects.begin(),resources.scene.effects.begin()+resources.scene.effectCount,
            [](const auto& effect) { return effect.desc.phase==1; });
        uint32_t guiGroup=0;
        const auto guiDepth=resources.scene.guiGroupCount?makeTexture(TextureFormat::D32_FLOAT,depthUsage):radray::RDGTextureHandle{};
        const auto guiRange=[&](uint32_t first,uint32_t last) {
            uint32_t begin=first; bool clearDepth=true;
            for(uint32_t i=first;i<last;++i) if(resources.gui.draws[i].desc.pipeline==RN_GUI_SCENE) {
                _gui->AddPass(graph,slot,resources.gui,color,width,height,resources.textureImports,begin,i-begin);
                _scene->AddPass(graph,slot,resources.scene,RN_SCENE_GUI,color,guiDepth,width,height,resources.textureImports,{},{},clearDepth,&resources.scene.guiGroups[guiGroup++]);
                clearDepth=false; begin=i+1;
            }
            _gui->AddPass(graph,slot,resources.gui,color,width,height,resources.textureImports,begin,last-begin);
        };
        guiRange(0,blur?f.gui_blur_split:uint32_t(resources.gui.draws.size()));
        if(blur) {
            _scene->AddPostEffects(graph,slot,resources.scene,1,color,depth,width,height);
            guiRange(f.gui_blur_split,uint32_t(resources.gui.draws.size()));
        }
        capture(0); capture(4);
        if(destination.IsValid()) _output->AddPass(graph,resources.output,*resources.color,color,destination,width,height);
    };
    if(resources.scene.phaseCounts[RN_SCENE_SKY]) {
        struct Clear {};
        graph.AddRasterPass("Raynium Scene Clear",Clear{},+[](const Clear&,radray::RDGRasterPassContext&) noexcept { return true; },
            +[](Clear&,radray::RDGPrepareContext&) noexcept { return true; })
            .WriteColor(0,color,LoadAction::Clear,StoreAction::Store,{{f.clear_color[0],f.clear_color[1],f.clear_color[2],f.clear_color[3]}})
            .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Clear,StoreAction::Store,LoadAction::DontCare,StoreAction::Discard,{0,0});
        scene(RN_SCENE_SKY);
    }
    geometry(Solid,color,{},!resources.scene.phaseCounts[RN_SCENE_SKY],true,{},-1); geometry(Cutout,color,{},false,true,{},-1);
    scene(RN_SCENE_SOLID); scene(RN_SCENE_TRANSLUCENT);
    if(!(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) scene(RN_SCENE_WATER_MASK);
    if(!input.hasTranslucent && !resources.scene.phaseCounts[RN_SCENE_OIT_BOUNDS] && !resources.scene.phaseCounts[RN_SCENE_CLOUD_BOUNDS]) { gui(); return; }
    if(!(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) { geometry(Translucent,color,{},false,true,{},-1); gui(); return; }
    const auto use=TextureUse::RenderTarget|TextureUse::Resource|TextureUse::CopySource|TextureUse::CopyDestination;
    const auto bounds=makeTexture(TextureFormat::RGBA32_FLOAT,use),culled=makeTexture(TextureFormat::RGBA32_FLOAT,use);
    const auto coeff0=makeTexture(TextureFormat::RGBA16_FLOAT,use),coeff1=makeTexture(TextureFormat::RGBA16_FLOAT,use),accumulate=makeTexture(TextureFormat::RGBA16_FLOAT,use);
    const bool clouds=resources.scene.phaseCounts[RN_SCENE_CLOUD_BOUNDS]!=0;
    const auto cloudDepth=clouds?makeTexture(TextureFormat::D32_FLOAT,depthUsage):radray::RDGTextureHandle{};
    if(resources.scene.phaseCounts[RN_SCENE_WATER_MASK]) geometryDepth=makeTexture(TextureFormat::D32_FLOAT,depthUsage);
    const auto waterMask=[&] {
        if(!resources.scene.phaseCounts[RN_SCENE_WATER_MASK]) return;
        _scene->AddDepthCopy(graph,resources.scene,depth,geometryDepth,width,height);
        _scene->AddPass(graph,slot,resources.scene,RN_SCENE_WATER_MASK,color,geometryDepth,width,height,resources.textureImports);
    };
    waterMask();
    geometry(OitBounds,bounds,{},true,false,{},-1);
    _scene->AddPass(graph,slot,resources.scene,RN_SCENE_OIT_BOUNDS,bounds,depth,width,height,resources.textureImports);
    copyTexture("Raynium CopyDepthBounds",bounds,culled);
    const auto screen=[&](uint32_t passId,radray::RDGTextureHandle destination,radray::array<radray::RDGTextureHandle,4> reads,int bindingIndex) {
        auto pass=graph.AddRasterPass(TerrainPrograms[passId],PassData{&resources,_resources->Pipeline(passId,TextureFormat::RGBA8_UNORM),input.lists,
            passId,width,height,_nativeLimit,bindingIndex,false,timing,true,constants,{},{},reads,&frame.timingMask},record,prepare);
        pass.ReadBuffer(constants,BufferState::CBuffer).WriteColor(0,destination,LoadAction::Load,StoreAction::Store)
            .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Load,StoreAction::Store);
        for(auto handle:reads) if(handle.IsValid()) pass.ReadTexture(handle);
    };
    screen(OitCull,culled,{bounds},0);
    if(clouds) {
        _scene->AddDepthCopy(graph,resources.scene,depth,cloudDepth,width,height);
        _scene->AddPass(graph,slot,resources.scene,RN_SCENE_CLOUD_BOUNDS,culled,cloudDepth,width,height,resources.textureImports);
    }
    waterMask(); geometry(OitTransmittance,coeff0,coeff1,true,false,{culled},1);
    _scene->AddPass(graph,slot,resources.scene,RN_SCENE_OIT_TRANSMITTANCE,coeff0,depth,width,height,resources.textureImports,coeff1,{culled});
    if(clouds) _scene->AddPass(graph,slot,resources.scene,RN_SCENE_CLOUD_TRANSMITTANCE,coeff0,cloudDepth,width,height,resources.textureImports,coeff1,{culled});
    geometry(OitAccumulate,accumulate,{},true,false,{culled,coeff0,coeff1},2);
    _scene->AddPass(graph,slot,resources.scene,RN_SCENE_OIT_ACCUMULATE,accumulate,depth,width,height,resources.textureImports,{}, {culled,coeff0,coeff1});
    if(clouds) _scene->AddPass(graph,slot,resources.scene,RN_SCENE_CLOUD_ACCUMULATE,accumulate,cloudDepth,width,height,resources.textureImports,{}, {culled,coeff0,coeff1});
    screen(OitComposite,color,{bounds,coeff0,coeff1,accumulate},3); gui();
} catch(...) { AbortUnexpectedException("SolidFramePipeline::SetupGraph"); }

void SolidFramePipeline::SubmitFrame(radray::FrameSlot& slot,CommandBuffer& graphCommands) noexcept try {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SubmitHandoff");
    if(_submitted==std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Terrain completion sequence exhausted");
    if(slot.GetIndex()>=_frames.size() || !_frames[slot.GetIndex()]) RADRAY_ABORT("Missing terrain submit frame: slot={}",slot.GetIndex());
    auto& frame=*_frames[slot.GetIndex()];
    auto& input=frame.input; const auto& f=input.parameters;
    auto& resources=_slots[slot.GetIndex()];
    const bool timing=(f.flags&(RN_FRAME_PROFILE|RN_FRAME_MEASURE))!=0 && resources.timestamps && resources.timing;
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
    for(auto& capture:resources.captures) capture->completion=frame.completion;
    if(input.color || !resources.captures.empty() || f.render_width) _lastColor=resources.color;
    for(auto* texture:resources.textureImports) texture->PublishSubmittedState();
    if(resources.scene.metrics) {
        resources.metrics.terrain_records=frame.submittedRecords;
        resources.metrics.terrain_calls=frame.directCalls+frame.indirectCalls;
        resources.metrics.bind_calls+=frame.bindingChanges;
    }
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
    if((frame->flags&(RN_FRAME_PROFILE|RN_FRAME_MEASURE)) && resources.timestamps && resources.timing) {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.ReadGpuTiming");
        auto* ticks=static_cast<uint64_t*>(resources.timing->Map(0,144));
        if(ticks) {
            resources.timing->InvalidateMappedRange({0,144});
            const double sample=double(ticks[1]-ticks[0])*resources.timestamps->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs/1e6;
            const auto period=resources.timestamps->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs/1e6;
            _gpuSequence=frame->sequence; _gpuNanoseconds=uint64_t(sample*1e6);
            if(frame->flags&RN_FRAME_PROFILE) {
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
            if(_gpuSampleCount==_gpuSamples.size()) {
                std::sort(_gpuSamples.begin(),_gpuSamples.end());
                RADRAY_INFO_LOG("Raynium terrain GPU samples=120 ms_p50={} p95={} p99={}",_gpuSamples[60],_gpuSamples[114],_gpuSamples[118]);
                _gpuSampleCount=0;
            }
            }
            resources.timing->Unmap();
        } else RADRAY_WARN_LOG("Cannot read completed terrain timestamps");
    }
    _frames[slot.GetIndex()]=nullptr;
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::Wait");
}
}
