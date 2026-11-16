bool ScenePassResources::PrepareTracy(Device& device,SceneFrameSlot& frame) {
    auto& draw=frame.tracy;
    if(!draw.pipeline) {
        draw.pipeline=GetPipeline(device,184,false,true);
        if(!draw.pipeline) return false;
    }
    for(uint32_t group=0;group<2;++group) if(!draw.parameters[group]) {
        draw.parameters[group]=device.CreateShaderParameterSet({draw.pipeline->program->groups[group].get()}).Release();
        if(!draw.parameters[group]) return false;
    }
    draw.graphSampler=GetTextureSampler(device,_samplers,12,{0,0,0,1});
    return draw.graphSampler!=nullptr;
}
radray::RDGTextureHandle ScenePassResources::AddTracy(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame,
    radray::RDGTextureHandle source,uint32_t width,uint32_t height) {
    const auto target=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,
        .MipLevels=1,.SampleCount=1,.Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::CopySource});
    const float clip=slot.GetDevice().GetBackend()==RenderBackend::Vulkan?1.f:-1.f;
    const auto backend=graph.CreateBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,clip,
        +[](const float& clip,radray::RDGUploadWriteContext& context) noexcept {
            std::memset(context.Bytes.data(),0,context.Bytes.size()); std::memcpy(context.Bytes.data(),&clip,sizeof(clip)); return true;
        }));
    struct Data { SceneDraw* draw; radray::RDGTextureHandle source; radray::RDGBufferHandle backend; uint32_t width,height; };
    graph.AddRasterPass("Raynium Tracy Image",Data{&frame.tracy,source,backend,width,height},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            return RecordSceneScreen(*data.draw,data.width,data.height,context.GetEncoder());
        },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
            auto& draw=*data.draw;
            struct Cleanup { SceneDraw* draw; ~Cleanup() noexcept { for(auto& p:draw->parameters) p->ResetBindingsForReuse(); } };
            context.GetSlot().Emplace<Cleanup>(&draw);
            for(auto& group:draw.parameters) group->ResetBindingsForReuse();
            const auto slice=context.ResolveBuffer(data.backend);
            const auto view=context.ResolveView(data.source,TextureViewUsage::Resource);
            return view && draw.parameters[0]->Set(31,0,ShaderBufferBinding{slice.Buffer.Get(),{slice.Offset,256}})
                && draw.parameters[1]->Set(0,0,view.Get()) && draw.parameters[1]->Set(1,0,draw.graphSampler)
                && draw.parameters[0]->FlushWrites() && draw.parameters[1]->FlushWrites();
        }).ReadTexture(source).ReadBuffer(backend,BufferState::CBuffer).WriteColor(0,target,LoadAction::DontCare,StoreAction::Store);
    return target;
}
bool ScenePassResources::PreparePostEffects(Device& device,SceneFrameSlot& frame,const rn_render_frame_desc& input) {
    frame.effectCount=0;
    if(input.scene_effect_count>64 || (input.scene_effect_count && !input.scene_effects) || input.gui_blur_split>input.gui_draw_count) {
        RADRAY_ERR_LOG("Invalid post effect array or GUI blur boundary"); return false;
    }
    const uint32_t count=input.scene_effect_count+(frame.phaseCounts[RN_SCENE_OUTLINE]?1u:0u);
    if(!count) return true;
    while(frame.effects.size()<count) frame.effects.emplace_back();
    bool gui=false;
    for(uint32_t i=0;i<count;++i) {
        const auto desc=i<input.scene_effect_count?input.scene_effects[i]:rn_scene_effect{3,2,UINT32_MAX,0};
        auto& effect=frame.effects[i];
        if(desc.reserved || (i<input.scene_effect_count && (desc.chain==3 || desc.phase>1 || desc.globals_blob>=frame.blobs.size() ||
            frame.blobs[desc.globals_blob].size!=256 || (desc.phase==1 && (desc.chain!=1 || gui)))) || !DescribePostChain(desc.chain,effect.chain)) {
            RADRAY_ERR_LOG("Invalid built-in post chain or globals: {}",desc.chain); return false;
        }
        gui|=desc.phase==1; effect.desc=desc;
        while(effect.passes.size()<effect.chain.passes.size()) effect.passes.emplace_back();
        for(size_t j=0;j<effect.chain.passes.size();++j) {
            auto& pass=effect.passes[j]; pass.description=&effect.chain.passes[j];
            auto* pipeline=GetPipeline(device,pass.description->pipeline); if(!pipeline) return false;
            auto& draw=pass.draw;
            if(draw.pipeline!=pipeline) {
                for(auto& parameters:draw.parameters) parameters.reset();
                draw.pipeline=pipeline;
                for(uint32_t group=0;group<2;++group) if(pipeline->program->groups[group]) {
                    draw.parameters[group]=device.CreateShaderParameterSet({pipeline->program->groups[group].get()}).Release();
                    if(!draw.parameters[group]) return false;
                }
            }
        }
    }
    for(uint32_t bits:{0u,12u}) {
        auto& sampler=_samplers[(1ull<<32)|bits];
        if(!sampler) sampler=device.CreateSampler({.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
            .MinFilter=bits?FilterMode::Linear:FilterMode::Nearest,.MagFilter=bits?FilterMode::Linear:FilterMode::Nearest,
            .MipmapFilter=FilterMode::Nearest,.LodMax=0,.AnisotropyClamp=1}).Release();
        if(!sampler) return false;
    }
    frame.effectCount=count; return true;
}
void ScenePassResources::AddPostEffects(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame,uint32_t phase,
    radray::RDGTextureHandle color,radray::RDGTextureHandle depth,uint32_t width,uint32_t height,radray::RDGTextureHandle composite) {
    struct Constant { uint32_t binding; radray::RDGBufferHandle buffer; };
    struct Texture { uint32_t binding; radray::RDGTextureHandle texture; Sampler* sampler; };
    struct Data {
        SceneDraw* draw; uint32_t width,height,constantCount{},textureCount{};
        radray::RDGBufferHandle backend;
        radray::array<Constant,8> constants{};
        radray::array<Texture,4> textures{};
        rn_frame_metrics* metrics{};
        uint32_t backendOffset{};
    };
    for(auto& effect:std::span{frame.effects}.first(frame.effectCount)) {
        if(effect.desc.phase!=phase) continue;
        std::pmr::vector<radray::RDGTextureHandle> targets{&slot.GetScratchMemoryResource()}; targets.push_back(color);
        for(uint32_t i=1;i<effect.chain.targetCount;++i) targets.push_back(phase==2 && i==2?composite:
            graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,
                .Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::Resource}));
        for(auto& pass:std::span{effect.passes}.first(effect.chain.passes.size())) {
            const auto& desc=*pass.description; auto& draw=pass.draw;
            Data data{&draw,width,height,0,0,frame.backend};
            data.metrics=frame.metrics;
            data.backendOffset=desc.pipeline>=10000?0:256;
            for(const auto& constant:desc.constants) {
                if(!(draw.pipeline->program->activeBindings[0]&(1ull<<constant.binding))) continue;
                radray::RDGBufferHandle buffer;
                if(constant.kind==1) buffer=frame.buffers[effect.desc.globals_blob];
                else {
                    auto values=constant.values;
                    if(constant.kind==2) for(size_t i=0;i<=desc.inputs.size();++i) { values[i*2]=float(width); values[i*2+1]=float(height); }
                    buffer=graph.CreateBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,values,
                        +[](const radray::array<float,64>& values,radray::RDGUploadWriteContext& context) noexcept {
                            std::memcpy(context.Bytes.data(),values.data(),256); return true;
                        }));
                    if(frame.metrics) frame.metrics->scene_upload_bytes+=256;
                }
                data.constants[data.constantCount++]={constant.binding,buffer};
            }
            for(const auto& source:desc.inputs) if(draw.pipeline->program->activeBindings[1]&(1ull<<source.binding))
                data.textures[data.textureCount++]={source.binding,targets[source.target],_samplers[(1ull<<32)|(source.linear?12u:0u)].get()};
            auto raster=graph.AddRasterPass("Raynium Post Effect",data,+[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
                return RecordSceneScreen(*data.draw,data.width,data.height,context.GetEncoder());
            },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
                auto& draw=*data.draw;
                struct Cleanup { SceneDraw* draw; ~Cleanup() noexcept { for(auto& p:draw->parameters) if(p) p->ResetBindingsForReuse(); } };
                context.GetSlot().Emplace<Cleanup>(&draw);
                for(auto& group:draw.parameters) if(group) group->ResetBindingsForReuse();
                auto backend=context.ResolveBuffer(data.backend);
                if(!draw.parameters[0]->Set(31,0,ShaderBufferBinding{backend.Buffer.Get(),{backend.Offset+data.backendOffset,256}})) return false;
                for(const auto& constant:std::span{data.constants}.first(data.constantCount)) {
                    auto slice=context.ResolveBuffer(constant.buffer);
                    if(!draw.parameters[0]->Set(constant.binding,0,ShaderBufferBinding{slice.Buffer.Get(),{slice.Offset,256}})) return false;
                }
                for(const auto& texture:std::span{data.textures}.first(data.textureCount)) {
                    auto view=context.ResolveView(texture.texture,TextureViewUsage::Resource);
                    if(!view || !draw.parameters[1]->Set(texture.binding,0,view.Get())) return false;
                    const auto& samplers=draw.pipeline->program->samplers;
                    if(std::find(samplers.begin(),samplers.end(),texture.binding+1)!=samplers.end()
                        && !draw.parameters[1]->Set(texture.binding+1,0,texture.sampler)) return false;
                }
                for(auto& group:draw.parameters) if(group && !group->FlushWrites()) return false;
                if(data.metrics) {
                    ++data.metrics->scene_calls;
                    data.metrics->bind_calls+=1+uint64_t(bool(draw.parameters[0]))+uint64_t(bool(draw.parameters[1]));
                }
                return true;
            });
            raster.WriteColor(0,targets[desc.output],phase==2 && desc.output==2?LoadAction::Load:LoadAction::Clear,StoreAction::Store)
                .SetDepthStencil(depth,TextureState::DepthRead,LoadAction::Load,StoreAction::Store)
                .ReadBuffer(frame.backend,BufferState::CBuffer);
            for(const auto& constant:std::span{data.constants}.first(data.constantCount)) raster.ReadBuffer(constant.buffer,BufferState::CBuffer);
            for(const auto& texture:std::span{data.textures}.first(data.textureCount)) raster.ReadTexture(texture.texture);
        }
    }
}
void ScenePassResources::AddOutline(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame,
    radray::RDGTextureHandle color,radray::RDGTextureHandle depth,uint32_t width,uint32_t height,radray::vector<TextureResource*>& imports) {
    if(!frame.phaseCounts[RN_SCENE_OUTLINE]) return;
    auto outline=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,
        .Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::Resource});
    struct Clear {};
    graph.AddRasterPass("Raynium Outline Clear",Clear{},+[](const Clear&,radray::RDGRasterPassContext&) noexcept { return true; },
        +[](Clear&,radray::RDGPrepareContext&) noexcept { return true; }).WriteColor(0,outline,LoadAction::Clear,StoreAction::Store);
    AddPass(graph,slot,frame,RN_SCENE_OUTLINE,outline,depth,width,height,imports);
    AddPostEffects(graph,slot,frame,2,outline,depth,width,height,color);
}
