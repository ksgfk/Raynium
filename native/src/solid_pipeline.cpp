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
using namespace radray::gpu;
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
    radray::array<SlotResources,2> slots;
    out=radray::unique_ptr<SolidFramePipeline>(new SolidFramePipeline(device,std::move(resources),std::move(gui),std::move(output),std::move(scene),std::move(slots),std::move(completion),indirect));
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
bool SolidFramePipeline::PrepareCapture(radray::FrameSlot& slot,const rn_render_frame_desc& frame,uint32_t width,uint32_t height) {
    auto& resources=_slots[slot.GetIndex()]; auto& device=slot.GetDevice();
    resources.profileStages=(frame.flags&RN_FRAME_PROFILE)!=0;
    const auto mode=resources.profileStages?radray::RDGGpuProfilingMode::FrameAndPasses:
        (frame.flags&RN_FRAME_MEASURE)?radray::RDGGpuProfilingMode::FrameOnly:radray::RDGGpuProfilingMode::Off;
    SetGpuProfiling(slot,mode,frame.output_sequence);
    if(!resources.color || resources.color->GetTexture()->GetDesc().Width!=width || resources.color->GetTexture()->GetDesc().Height!=height) {
        auto replacement=CreateSceneTexture(device,{sizeof(rn_uploaded_texture_desc),0,width,height,1,RN_PIXEL_RGBA8,1,RN_TEXTURE_ATTACHMENT|RN_TEXTURE_READBACK});
        if(!replacement) { RADRAY_ERR_LOG("Cannot create scene color target"); return false; }
        resources.color=std::move(replacement);
    }
    if(frame.flags&RN_FRAME_CAPTURE) {
        const rn_capture_request request{sizeof(rn_capture_request),(frame.flags&RN_FRAME_CAPTURE_PREVIOUS)?1u:0u,frame.output_sequence};
        if(!RequestCapture({},request)) return false;
        _captureRequests.back().inlineRequest=true;
    }
    for(auto& queued:_captureRequests) {
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
        queued.width=captureWidth; queued.height=captureHeight;
        if(!_readback.ReserveTexture(request.id,{captureWidth,captureHeight,TextureFormat::RGBA8_UNORM})) {
            RADRAY_ERR_LOG("Cannot reserve capture readback: id={}",request.id); return false;
        }
    }
    return true;
}
void SolidFramePipeline::CancelUnsubmittedCaptures() noexcept {
    _readback.CancelUnsubmitted();
    std::erase_if(_captureRequests,[](const auto& entry){ return entry.inlineRequest; });
}
bool SolidFramePipeline::RequestCapture(const TextureRegistry& textures,const rn_capture_request& request) {
    const auto fail=[] { RADRAY_ERR_LOG("Invalid capture request, duplicate ID, unavailable source or unread capture limit exceeded"); return false; };
    if(request.struct_size!=sizeof(request) || request.reserved || request.source>4 || !request.id || _readback.HasRequest(request.id) ||
        _readback.GetActiveRequestCount()+_captureRequests.size()>=4096 || std::any_of(_captureRequests.begin(),_captureRequests.end(),
            [&](const auto& queued) { return queued.request.id==request.id; })) return fail();
    radray::IntrusivePtr<radray::GpuTexture> texture;
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
    if(std::any_of(_captureRequests.begin(),_captureRequests.end(),[&](const auto& entry){ return entry.request.id==sequence; })) {
        info={sizeof(info)}; return true;
    }
    radray::ReadbackResultInfo result{};
    if(!_readback.Poll(sequence,_completion->GetCompletedValue(),result,static_cast<radray::byte*>(pixels),capacity)) {
        RADRAY_ERR_LOG("Cannot poll capture: id={} capacity={}",sequence,capacity); return false;
    }
    info={sizeof(info),result.Ready?1u:0u,result.Texture.Width,result.Texture.Height,result.PackedByteSize,uint32_t(result.PackedRowPitch),0};
    return true;
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
    FrameTextureImports imports{graph,slot.GetScratchMemoryResource()};
    input.textures->AddUploads(graph,imports,resources.scene.metrics);
    _scene->AddBufferUploads(graph,resources.scene.metrics);
    _scene->Upload(graph,slot,resources.scene);
    _scene->AddOffscreenPasses(graph,slot,resources.scene,imports);
    if(!input.color && _captureRequests.empty() && !f.render_width) return;
    frame.renderedSceneColor=true;
    const auto destination=input.color?graph.ImportTexture(input.color,TextureState::Undefined,TextureState::Present):radray::RDGTextureHandle{};
    const uint32_t width=input.width,height=input.height;
    const auto color=imports.Import(resources.color);
    graph.MarkOutput(color);
    if(destination.IsValid()) graph.MarkOutput(destination);
    const auto capture=[&](uint32_t stage) {
        radray::RDGTextureHandle tracy;
        std::erase_if(_captureRequests,[&](const CaptureSource& entry) {
            const auto& request=entry.request;
            if(request.source!=stage) return false;
            auto source=entry.texture?imports.Import(entry.texture):color;
            if(stage==4) {
                if(!tracy.IsValid()) tracy=_scene->AddTracy(graph,slot,resources.scene,source,entry.width,entry.height);
                source=tracy;
            }
            if(!_readback.EnqueueReserved(graph,request.id,source,request.mip,request.layer))
                RADRAY_ABORT("Cannot enqueue reserved capture: id={}",request.id);
            return true;
        });
    };
    capture(1); capture(3);
    struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4]; } values{};
    static_assert(sizeof(Constants)==192);
    std::copy_n(f.projection,16,values.projection); std::copy_n(f.view,16,values.view); std::copy_n(f.camera_block,3,values.block);
    values.block[3]=(f.flags&RN_FRAME_RGSS)!=0; std::copy_n(f.camera_fraction,3,values.fraction);
    values.fraction[3]=slot.GetDevice().GetBackend()==GpuBackend::Vulkan?1.f:-1.f;
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
        std::pmr::unordered_set<radray::GpuTexture*> textures{&slot.GetScratchMemoryResource()};
        for(const auto& batch:std::span{input.lists->batches}.subspan(draws.firstBatch,draws.batchCount)) {
            if(!batch.vertexPage || !batch.indexPage) RADRAY_ABORT("Missing terrain physical page");
            if(vertexPages.insert(batch.vertexPage).second) access.vertices.push_back(graph.ImportPersistentBorrowed(*batch.vertexPage));
            if(indexPages.insert(batch.indexPage).second) access.indices.push_back(graph.ImportPersistentBorrowed(*batch.indexPage));
            for(const auto& texture:batch.material->textures) if(texture && textures.insert(texture.Get()).second)
                access.textures.push_back(imports.Import(texture));
        }
    }
    struct PassData {
        const MeshPassResources* mesh;
        GraphicsPipelineState* pipeline;
        const TerrainFrameLists* lists;
        uint32_t passId,width,height,nativeLimit;
        bool hasOitTextures;
        bool useIndirect,screen;
        radray::RDGBufferHandle constants,instances,indirect;
        radray::array<radray::RDGTextureHandle,4> reads;
        radray::RDGBufferSlice instanceSlice{},indirectSlice{};
        ShaderParameterSet* passParameters{};
        ShaderParameterSet* oitParameters{};
    };
    const auto prepare=+[](PassData& data,radray::RDGPrepareContext& context) noexcept {
        try {
        if(data.passId!=OitComposite) {
            const auto constant=FrameConstantBinding(0,context.ResolveBuffer(data.constants));
            data.passParameters=context.GetSlot().AcquireParameterSet(data.mesh->PassGroupLayout(),{&constant,1}).Get();
            if(!data.passParameters) return false;
        }
        if(!data.screen) {
            data.instanceSlice=context.ResolveBuffer(data.instances);
            if(data.useIndirect) data.indirectSlice=context.ResolveBuffer(data.indirect);
        }
        if(data.hasOitTextures) {
            radray::array<radray::TransientParameterBinding,4> bindings;
            for(uint32_t i=0;i<bindings.size();++i) {
                const auto view=context.ResolveView(data.reads[i].IsValid()?data.reads[i]:data.reads[0],TextureViewUsage::Resource);
                if(!view) return false;
                bindings[i]={i,0,view.Get()};
            }
            data.oitParameters=context.GetSlot().AcquireParameterSet(data.mesh->OitGroupLayout(),bindings).Get();
            if(!data.oitParameters) return false;
        }
        return true;
        } catch(...) { AbortUnexpectedException("Raynium Terrain prepare"); }
    };
    const auto record=+[](const PassData& data,radray::RDGRasterPassContext& context) noexcept {
        try {
            RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Record");
            auto& encoder=context.GetEncoder();
            encoder.SetViewport({0,0,float(data.width),float(data.height),0,1}); encoder.SetScissor({0,0,data.width,data.height});
            encoder.BindGraphicsPipelineState(data.pipeline);
            if(data.passId!=OitComposite) encoder.BindShaderParameterSet(0,data.passParameters);
            if(data.hasOitTextures) encoder.BindShaderParameterSet(2,data.oitParameters);
            if(data.screen) encoder.Draw(3,1,0,0);
            else if(!RecordTerrainDraws({data.lists,data.passId<=Translucent?data.passId:uint32_t(Translucent),data.nativeLimit,
                data.useIndirect,data.instanceSlice,data.indirectSlice},encoder)) return false;
            return encoder.IsValid();
        } catch(...) { AbortUnexpectedException("Raynium Terrain record"); }
    };
    const auto geometry=[&](uint32_t passId,radray::RDGTextureHandle destination,radray::RDGTextureHandle second,bool clear,bool writeDepth,
        radray::array<radray::RDGTextureHandle,4> reads,bool hasOitTextures) {
        const auto layer=passId<=Translucent?passId:uint32_t(Translucent); const auto& draws=input.lists->layers[layer];
        resources.visibleCommands[passId]=draws.recordCount;
        frame.submittedIndices+=draws.indicesInstances; frame.submittedRecords+=draws.recordCount; frame.submittedBatches+=draws.batchCount;
        frame.bindingChanges+=2+uint64_t(hasOitTextures)+(!input.instances.empty()?1:0);
        const TerrainDrawBatch* previous=nullptr;
        for(const auto& batch:std::span{input.lists->batches}.subspan(draws.firstBatch,draws.batchCount)) {
            frame.bindingChanges+=!previous || previous->material->parameters.get()!=batch.material->parameters.get();
            frame.bindingChanges+=!previous || previous->vertices.Target!=batch.vertices.Target || previous->vertices.Offset!=batch.vertices.Offset || previous->vertices.Size!=batch.vertices.Size;
            frame.bindingChanges+=!previous || previous->indices.Target!=batch.indices.Target || previous->indices.Offset!=batch.indices.Offset || previous->indices.Stride!=batch.indices.Stride;
            if(_indirect) frame.indirectCalls+=(uint64_t(batch.count)+_nativeLimit-1)/_nativeLimit; else frame.directCalls+=batch.count;
            previous=&batch;
        }
        auto pass=graph.AddRasterPass(TerrainPrograms[passId],PassData{_resources.get(),_resources->Pipeline(passId,TextureFormat::RGBA8_UNORM,(f.flags&RN_FRAME_FLIP_Y)!=0,(f.flags&RN_FRAME_WIREFRAME)!=0),
            input.lists,passId,width,height,_nativeLimit,hasOitTextures,_indirect,false,constants,instances,indirect,reads},record,prepare);
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
    const auto scene=[&](rn_scene_phase phase) { _scene->AddPass(graph,slot,resources.scene,phase,color,depth,width,height,imports); };
    const auto gui=[&] {
        scene(RN_SCENE_AFTER_TERRAIN); scene(RN_SCENE_SEE_THROUGH);
        const auto overlay=[&](rn_scene_phase phase,int index) {
            if(!resources.scene.phaseCounts[phase]) return;
            const auto target=resources.scene.consistentDepth?makeTexture(TextureFormat::D32_FLOAT,depthUsage):depth;
            _scene->AddPass(graph,slot,resources.scene,phase,color,target,width,height,imports,{},{},true);
            if(resources.scene.consistentDepth) _scene->AddDepthCopy(graph,resources.scene,target,depth,width,height,index);
        };
        overlay(RN_SCENE_ALWAYS_ON_TOP,0); overlay(RN_SCENE_HAND,1);
        capture(2);
        if(f.flags&RN_FRAME_WORLD_ONLY) { capture(0); capture(4); return; }
        _scene->AddOutline(graph,slot,resources.scene,color,depth,width,height,imports);
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
                _gui->AddPass(graph,slot,resources.gui,color,width,height,imports,begin,i-begin);
                _scene->AddPass(graph,slot,resources.scene,RN_SCENE_GUI,color,guiDepth,width,height,imports,{},{},clearDepth,&resources.scene.guiGroups[guiGroup++]);
                clearDepth=false; begin=i+1;
            }
            _gui->AddPass(graph,slot,resources.gui,color,width,height,imports,begin,last-begin);
        };
        guiRange(0,blur?f.gui_blur_split:uint32_t(resources.gui.draws.size()));
        if(blur) {
            _scene->AddPostEffects(graph,slot,resources.scene,1,color,depth,width,height);
            guiRange(f.gui_blur_split,uint32_t(resources.gui.draws.size()));
        }
        capture(0); capture(4);
        if(destination.IsValid()) _output->AddPass(graph,color,destination,width,height);
    };
    if(resources.scene.phaseCounts[RN_SCENE_SKY]) {
        graph.AddClearColor(color,{{f.clear_color[0],f.clear_color[1],f.clear_color[2],f.clear_color[3]}});
        graph.AddClearDepthStencil(depth,{0,0},true,false);
        scene(RN_SCENE_SKY);
    }
    geometry(Solid,color,{},!resources.scene.phaseCounts[RN_SCENE_SKY],true,{},false); geometry(Cutout,color,{},false,true,{},false);
    scene(RN_SCENE_SOLID); scene(RN_SCENE_TRANSLUCENT);
    if(!(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) scene(RN_SCENE_WATER_MASK);
    if(!input.hasTranslucent && !resources.scene.phaseCounts[RN_SCENE_OIT_BOUNDS] && !resources.scene.phaseCounts[RN_SCENE_CLOUD_BOUNDS]) { gui(); return; }
    if(!(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) { geometry(Translucent,color,{},false,true,{},false); gui(); return; }
    const auto use=TextureUse::RenderTarget|TextureUse::Resource|TextureUse::CopySource|TextureUse::CopyDestination;
    const auto bounds=makeTexture(TextureFormat::RGBA32_FLOAT,use),culled=makeTexture(TextureFormat::RGBA32_FLOAT,use);
    const auto coeff0=makeTexture(TextureFormat::RGBA16_FLOAT,use),coeff1=makeTexture(TextureFormat::RGBA16_FLOAT,use),accumulate=makeTexture(TextureFormat::RGBA16_FLOAT,use);
    const bool clouds=resources.scene.phaseCounts[RN_SCENE_CLOUD_BOUNDS]!=0;
    const auto cloudDepth=clouds?makeTexture(TextureFormat::D32_FLOAT,depthUsage):radray::RDGTextureHandle{};
    if(resources.scene.phaseCounts[RN_SCENE_WATER_MASK]) geometryDepth=makeTexture(TextureFormat::D32_FLOAT,depthUsage);
    const auto waterMask=[&] {
        if(!resources.scene.phaseCounts[RN_SCENE_WATER_MASK]) return;
        _scene->AddDepthCopy(graph,resources.scene,depth,geometryDepth,width,height);
        _scene->AddPass(graph,slot,resources.scene,RN_SCENE_WATER_MASK,color,geometryDepth,width,height,imports);
    };
    waterMask();
    geometry(OitBounds,bounds,{},true,false,{},false);
    _scene->AddPass(graph,slot,resources.scene,RN_SCENE_OIT_BOUNDS,bounds,depth,width,height,imports);
    copyTexture("Raynium CopyDepthBounds",bounds,culled);
    const auto screen=[&](uint32_t passId,radray::RDGTextureHandle destination,radray::array<radray::RDGTextureHandle,4> reads) {
        auto pass=graph.AddRasterPass(TerrainPrograms[passId],PassData{_resources.get(),_resources->Pipeline(passId,TextureFormat::RGBA8_UNORM),input.lists,
            passId,width,height,_nativeLimit,true,false,true,constants,{},{},reads},record,prepare);
        pass.ReadBuffer(constants,BufferState::CBuffer).WriteColor(0,destination,LoadAction::Load,StoreAction::Store)
            .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Load,StoreAction::Store);
        for(auto handle:reads) if(handle.IsValid()) pass.ReadTexture(handle);
    };
    screen(OitCull,culled,{bounds});
    if(clouds) {
        _scene->AddDepthCopy(graph,resources.scene,depth,cloudDepth,width,height);
        _scene->AddPass(graph,slot,resources.scene,RN_SCENE_CLOUD_BOUNDS,culled,cloudDepth,width,height,imports);
    }
    waterMask(); geometry(OitTransmittance,coeff0,coeff1,true,false,{culled},true);
    _scene->AddPass(graph,slot,resources.scene,RN_SCENE_OIT_TRANSMITTANCE,coeff0,depth,width,height,imports,coeff1,{culled});
    if(clouds) _scene->AddPass(graph,slot,resources.scene,RN_SCENE_CLOUD_TRANSMITTANCE,coeff0,cloudDepth,width,height,imports,coeff1,{culled});
    geometry(OitAccumulate,accumulate,{},true,false,{culled,coeff0,coeff1},true);
    _scene->AddPass(graph,slot,resources.scene,RN_SCENE_OIT_ACCUMULATE,accumulate,depth,width,height,imports,{}, {culled,coeff0,coeff1});
    if(clouds) _scene->AddPass(graph,slot,resources.scene,RN_SCENE_CLOUD_ACCUMULATE,accumulate,cloudDepth,width,height,imports,{}, {culled,coeff0,coeff1});
    screen(OitComposite,color,{bounds,coeff0,coeff1,accumulate}); gui();
} catch(...) { AbortUnexpectedException("SolidFramePipeline::SetupGraph"); }

void SolidFramePipeline::SubmitFrame(radray::FrameSlot& slot,CommandBuffer& graphCommands) noexcept try {
    RADRAY_PROFILE_SCOPE_N("Raynium.Frame.SubmitHandoff");
    if(_submitted==std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Terrain completion sequence exhausted");
    if(slot.GetIndex()>=_frames.size() || !_frames[slot.GetIndex()]) RADRAY_ABORT("Missing terrain submit frame: slot={}",slot.GetIndex());
    auto& frame=*_frames[slot.GetIndex()];
    auto& input=frame.input;
    auto& resources=_slots[slot.GetIndex()];
    CommandBuffer* commands[]{&graphCommands};
    radray::array<Fence*,1> signals{_completion.get()};
    radray::array<uint64_t,1> signalValues{_submitted+1};
    SwapChainSyncObject* acquire[]{input.waitToDraw}; SwapChainSyncObject* present[]{input.readyToPresent};
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Frame.QueueSubmit");
        slot.GetQueue().Submit({.CmdBuffers=commands,.SignalFences=signals,.SignalValues=signalValues,
            .WaitToExecute={acquire,input.waitToDraw?1u:0u},.ReadyToPresent={present,input.readyToPresent?1u:0u}});
    }
    frame.completion=++_submitted;
    _readback.MarkSubmitted(frame.completion);
    if(frame.renderedSceneColor) _lastColor=resources.color;
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
    _frames[slot.GetIndex()]=nullptr;
} catch(...) {
    AbortUnexpectedException("SolidFramePipeline::Wait");
}
void SolidFramePipeline::OnGpuTimingCompleted(radray::FrameSlot& slot,const radray::RDGGpuTimingSnapshot& snapshot) noexcept {
    _gpuSequence=snapshot.UserSequence; _gpuNanoseconds=snapshot.TotalNanoseconds;
    const auto& resources=_slots[slot.GetIndex()];
    if(!resources.profileStages) return;
    for(const auto& timing:snapshot.Passes) {
        const auto found=std::find(TerrainPrograms.begin(),TerrainPrograms.end(),std::string_view{timing.Name});
        if(found==TerrainPrograms.end()) continue;
        const auto pass=size_t(found-TerrainPrograms.begin());
        auto& samples=_stageSamples[pass]; auto& count=_stageCounts[pass];
        samples[count++]=double(timing.Nanoseconds)/1e6;
        if(count==samples.size()) {
            std::sort(samples.begin(),samples.end());
            RADRAY_INFO_LOG("Terrain GPU stage={} samples=120 ms_p50={} p95={} p99={} visible_commands={}",*found,samples[60],samples[114],samples[118],pass<6?resources.visibleCommands[pass]:1);
            count=0;
        }
    }
    const double sample=double(_gpuNanoseconds)/1e6;
    _gpuSamples[_gpuSampleCount++]=sample;
    RADRAY_PROFILE_PLOT("Raynium.GpuFrame.Sequence",double(_gpuSequence));
    RADRAY_PROFILE_PLOT("Raynium.GpuMilliseconds",sample);
    if(_gpuSampleCount==_gpuSamples.size()) {
        std::sort(_gpuSamples.begin(),_gpuSamples.end());
        RADRAY_INFO_LOG("Raynium terrain GPU samples=120 ms_p50={} p95={} p99={}",_gpuSamples[60],_gpuSamples[114],_gpuSamples[118]);
        _gpuSampleCount=0;
    }
}
}
