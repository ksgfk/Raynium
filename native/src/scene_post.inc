bool ScenePassResources::PrepareTracy(Device& device,SceneFrameSlot& frame) {
    auto& draw=frame.tracy;
    if(!draw.pipeline) {
        draw.pipeline=GetPipeline(device,184,false,true);
        if(!draw.pipeline) return false;
    }
    draw.graphSampler=GetTextureSampler(device,12,{0,0,0,1});
    return draw.graphSampler!=nullptr;
}
radray::RDGTextureHandle ScenePassResources::AddTracy(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame,
    radray::RDGTextureHandle source,uint32_t width,uint32_t height) {
    const auto target=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,
        .MipLevels=1,.SampleCount=1,.Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::CopySource});
    const float clip=slot.GetDevice().GetBackend()==GpuBackend::Vulkan?1.f:-1.f;
    const auto backend=graph.CreateBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,clip,
        +[](const float& clip,radray::RDGUploadWriteContext& context) noexcept {
            std::memset(context.Bytes.data(),0,context.Bytes.size()); std::memcpy(context.Bytes.data(),&clip,sizeof(clip)); return true;
        }));
    struct Data { SceneDraw* draw; radray::RDGTextureHandle source; radray::RDGBufferHandle backend; uint32_t width,height; SceneBindings groups{}; };
    graph.AddRasterPass("Raynium Tracy Image",Data{&frame.tracy,source,backend,width,height},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            return RecordSceneScreen(*data.draw,data.groups,data.width,data.height,context.GetEncoder());
        },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
            return PrepareScreenBindings(context,*data.draw,data.backend,0,data.source,data.groups);
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
            pass.draw.pipeline=pipeline;
        }
    }
    if(!GetPostSampler(device,false) || !GetPostSampler(device,true)) return false;
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
        SceneBindings groups{};
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
                data.textures[data.textureCount++]={source.binding,targets[source.target],GetPostSampler(slot.GetDevice(),source.linear)};
            auto raster=graph.AddRasterPass("Raynium Post Effect",data,+[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
                return RecordSceneScreen(*data.draw,data.groups,data.width,data.height,context.GetEncoder());
            },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
                auto& program=*data.draw->pipeline->program;
                radray::array<radray::TransientParameterBinding,9> constants;
                radray::array<radray::TransientParameterBinding,8> textures;
                auto backend=context.ResolveBuffer(data.backend); backend.Offset+=data.backendOffset; backend.Size=256;
                constants[0]=FrameConstantBinding(31,backend);
                size_t constantCount=1,textureCount=0;
                for(const auto& constant:std::span{data.constants}.first(data.constantCount)) {
                    auto slice=context.ResolveBuffer(constant.buffer); slice.Size=256;
                    constants[constantCount++]=FrameConstantBinding(constant.binding,slice);
                }
                for(const auto& texture:std::span{data.textures}.first(data.textureCount)) {
                    auto view=context.ResolveView(texture.texture,TextureViewUsage::Resource);
                    if(!view) return false;
                    textures[textureCount++]={texture.binding,0,view.Get()};
                    if(UsesSampler(program,texture.binding+1)) textures[textureCount++]={texture.binding+1,0,texture.sampler};
                }
                if(!AcquireSceneBindings(context.GetSlot(),program,std::span{constants}.first(constantCount),
                    std::span{textures}.first(textureCount),data.groups)) return false;
                if(data.metrics) {
                    ++data.metrics->scene_calls;
                    data.metrics->bind_calls+=1+uint64_t(bool(data.groups.constants))+uint64_t(bool(data.groups.textures));
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
    radray::RDGTextureHandle color,radray::RDGTextureHandle depth,uint32_t width,uint32_t height,FrameTextureImports& imports) {
    if(!frame.phaseCounts[RN_SCENE_OUTLINE]) return;
    auto outline=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=width,.Height=height,.DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,
        .Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::Resource});
    graph.AddClearColor(outline,{});
    AddPass(graph,slot,frame,RN_SCENE_OUTLINE,outline,depth,width,height,imports);
    AddPostEffects(graph,slot,frame,2,outline,depth,width,height,color);
}
