#include <raynium/scene_pass.h>
#include <raynium/frame_record.h>
#include <raynium/terrain_draw.h>
#include <raynium/exception.h>
#include <radray/logger.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace raynium {
using namespace radray::render;
namespace {
#include "scene_catalog.inc"
#include "post_catalog.inc"
bool Range(uint64_t offset,uint64_t size,uint64_t extent) { return offset<=extent && size<=extent-offset; }
ShaderStages Stages(uint32_t mask) {
    ShaderStages stages{};
    if(mask&(1u<<uint8_t(radray::shader::ShaderStage::Vertex))) stages|=ShaderStage::Vertex;
    if(mask&(1u<<uint8_t(radray::shader::ShaderStage::Pixel))) stages|=ShaderStage::Pixel;
    return stages;
}
}
bool ScenePassResources::Create(std::span<const radray::byte> bytes,radray::unique_ptr<ScenePassResources>& result) {
    radray::vector<ShaderPackEntry> entries;
    if(result || !ReadShaderPack(bytes,entries)) return false;
    result=radray::unique_ptr<ScenePassResources>(new ScenePassResources({bytes.begin(),bytes.end()})); return true;
}
bool ScenePassResources::CreateBuffer(Device& device,const rn_scene_buffer_desc& desc) {
    if(desc.struct_size!=sizeof(desc) || desc.id<=_lastBufferId || desc.id>=0x7fffffffu || !desc.size || desc.size>(1ull<<30)) {
        RADRAY_ERR_LOG("Invalid scene buffer size or non-monotonic ID: {}",desc.id); return false;
    }
    auto buffer=radray::GpuBuffer::Create(device,{desc.size,BufferUse::Vertex|BufferUse::Index|BufferUse::Resource}).Release();
    if(!buffer) { RADRAY_ERR_LOG("Cannot create scene buffer: {}",desc.id); return false; }
    _buffers.emplace(desc.id,std::move(buffer)); _lastBufferId=desc.id; return true;
}
bool ScenePassResources::UploadBuffer(const rn_scene_buffer_upload_desc& desc) {
    auto found=_buffers.find(desc.id);
    if(desc.struct_size!=sizeof(desc) || found==_buffers.end() || (desc.size && !desc.bytes) ||
        !Range(desc.offset,desc.size,found->second->GetDesc().Size)) {
        RADRAY_ERR_LOG("Invalid scene buffer upload: {}",desc.id); return false;
    }
    if(desc.size) {
        BufferUpload upload{found->second,desc.offset,{}}; upload.bytes.resize(size_t(desc.size));
        std::memcpy(upload.bytes.data(),desc.bytes,size_t(desc.size)); _uploads.push_back(std::move(upload));
    }
    return true;
}
bool ScenePassResources::DestroyBuffer(uint32_t id) {
    if(!_buffers.erase(id)) { RADRAY_ERR_LOG("Scene buffer ID is missing or destroyed: {}",id); return false; }
    return true;
}
void ScenePassResources::AddBufferUploads(radray::RDGBuilder& graph,rn_frame_metrics* metrics) {
    for(auto& upload:_uploads) {
        if(metrics) metrics->scene_upload_bytes+=upload.bytes.size();
        const auto handle=graph.ImportPersistentOwned(upload.target);
        graph.UploadBuffer(handle,upload.offset,radray::RDGUploadSource::OwnedBytes(std::move(upload.bytes))); graph.MarkOutput(handle);
    }
    _uploads.clear();
}
ScenePipeline* ScenePassResources::GetPipeline(Device& device,uint32_t id,bool flipped,bool colorOnly) {
    const uint32_t key=id|(flipped?0x80000000u:0)|(colorOnly?0x40000000u:0);
    if(auto found=_pipelines.find(key);found!=_pipelines.end()) return found->second.get();
    auto pipeline=radray::make_unique<ScenePipeline>(); auto& desc=pipeline->description;
    if(!DescribeScenePipeline(id,desc) && !DescribePostPipeline(id,desc)) { RADRAY_ERR_LOG("Unknown vanilla pipeline id={}",id); return nullptr; }
    const bool vk=device.GetBackend()==RenderBackend::Vulkan;
    auto& cached=_programs[radray::string(desc.program)];
    if(!cached) {
        auto program=radray::make_unique<SceneProgram>();
        if(!ShaderProgram::Create(_bytes,desc.program,vk?radray::shader::ShaderTarget::SPIRV:radray::shader::ShaderTarget::DXIL,program->program)) return nullptr;
        const auto& artifact=program->program->Artifact();
        if(!artifact.RootConstants().empty() || !artifact.Samplers().empty()) { RADRAY_ERR_LOG("Unexpected vanilla shader root/static bindings"); return nullptr; }
        radray::array<radray::vector<BindingDescriptor>,2> bindings;
        for(const auto& binding:artifact.Bindings()) {
            const auto kind=static_cast<radray::shader::ShaderBindingKind>(binding.Type);
            const bool cb=kind==radray::shader::ShaderBindingKind::CBuffer,texture=kind==radray::shader::ShaderBindingKind::Texture;
            const bool texel=kind==radray::shader::ShaderBindingKind::TypedBuffer;
            if(binding.Group!=(cb?0u:1u) || binding.Count!=1 || (!cb && !texture && !texel && kind!=radray::shader::ShaderBindingKind::Sampler)) {
                RADRAY_ERR_LOG("Unsupported vanilla shader binding: {}",desc.program); return nullptr;
            }
            const uint32_t logical=cb || vk?binding.Binding:binding.Binding*2+(texture || texel?0u:1u);
            bindings[binding.Group].push_back({logical,cb?BindingKind::CBuffer:texture?BindingKind::Texture:texel?BindingKind::TypedBuffer:BindingKind::Sampler,1,Stages(binding.StageMask),
                D3D12BindingDescriptor{cb?BindingRegisterClass::CBV:texture || texel?BindingRegisterClass::SRV:BindingRegisterClass::Sampler,
                    cb?logical:logical/2,binding.Group},VulkanBindingDescriptor{binding.Group,logical}});
            if(cb || texture || texel) { program->required[binding.Group].push_back(logical); program->activeBindings[binding.Group]|=1ull<<logical; }
            else program->samplers.push_back(logical);
            if(texel) program->texelBindings|=1ull<<logical;
        }
        radray::vector<PipelineLayoutGroupDescriptor> groups;
        for(uint32_t group=0;group<2;++group) if(!bindings[group].empty()) {
            program->groups[group]=device.CreateBindingGroupLayout({bindings[group]}).Release();
            if(!program->groups[group]) return nullptr;
            groups.push_back({group,program->groups[group].get()});
        }
        program->layout=device.CreatePipelineLayout({groups,{}}).Release();
        const auto blob=vk?ShaderBlobCategory::SPIRV:ShaderBlobCategory::DXIL;
        program->vs=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex),blob,ShaderStage::Vertex}).Release();
        program->ps=device.CreateShader({*artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel),blob,ShaderStage::Pixel}).Release();
        if(!program->layout || !program->vs || !program->ps) return nullptr;
        cached=std::move(program);
    }
    pipeline->program=cached.get(); const auto& artifact=cached->program->Artifact();
    radray::vector<VertexAttribute> attributes;
    for(const auto& input:artifact.VertexInputs()) {
        auto found=std::find_if(desc.attributes.begin(),desc.attributes.end(),[&](const auto& a){return a.SemanticIndex==input.SemanticIndex;});
        if(found==desc.attributes.end()) { RADRAY_ERR_LOG("Missing vanilla vertex input: pipeline={} semantic={}",id,input.SemanticIndex); return nullptr; }
        auto attribute=*found; attribute.Location=input.Location; attributes.push_back(attribute);
    }
    desc.primitive.UnclippedDepth=false;
    desc.primitive.FaceClockwise=(desc.screen!=flipped)?FrontFace::CW:FrontFace::CCW;
    desc.primitive.Topology=desc.topology==2?PrimitiveTopology::TriangleStrip:PrimitiveTopology::TriangleList;
    if(desc.topology==2) desc.primitive.StripIndexFormat=IndexFormat::UINT32;
    std::optional<DepthStencilState> depth=colorOnly?std::nullopt:
        std::optional{desc.depth.value_or(DepthStencilState{.Format=TextureFormat::D32_FLOAT,.DepthCompare=CompareFunction::Always})};
    pipeline->pipeline=device.CreateGraphicsPipelineState({.PipelineLayout=cached->layout.get(),
        .VS=ShaderEntry{cached->vs.get(),cached->program->Entry(radray::shader::ShaderStage::Vertex)},
        .PS=ShaderEntry{cached->ps.get(),cached->program->Entry(radray::shader::ShaderStage::Pixel)},
        .VertexInput={desc.vertices,attributes},.Primitive=desc.primitive,.DepthStencil=depth,
        .MultiSample=MultiSampleState::Default(),.ColorTargets=desc.colors}).Release();
    if(!pipeline->pipeline) { RADRAY_ERR_LOG("Cannot create vanilla pipeline: {}",id); return nullptr; }
    auto* result=pipeline.get(); _pipelines.emplace(key,std::move(pipeline)); return result;
}
bool ScenePassResources::Prepare(Device& device,SceneFrameSlot& slot,const UploadedTextures& textures,const rn_render_frame_desc& frame) {
    slot.ClearBorrowed(); slot.drawCount=0; slot.targetCount=0; slot.phaseCounts.fill(0);
    const auto fail=[] { RADRAY_ERR_LOG("Invalid scene arrays, geometry, bindings or phase"); return false; };
    if(frame.scene_reserved || frame.scene_target_reserved || frame.scene_target_count>65536 || frame.scene_blob_count>1048576 || frame.scene_binding_count>4194304 || frame.scene_draw_count>1048576 ||
        (frame.scene_blob_count && !frame.scene_blobs) || (frame.scene_binding_count && !frame.scene_bindings) || (frame.scene_draw_count && !frame.scene_draws) ||
        (frame.scene_target_count && !frame.scene_targets)) return fail();
    while(slot.targets.size()<frame.scene_target_count) slot.targets.emplace_back();
    uint32_t targetEnd=0;
    for(uint32_t i=0;i<frame.scene_target_count;++i) {
        const auto& input=frame.scene_targets[i]; const auto* texture=textures.Find(input.texture);
        if(!texture || input.reserved || !(texture->desc.flags&RN_TEXTURE_ATTACHMENT) || texture->desc.flags&RN_TEXTURE_CUBE ||
            texture->desc.format!=RN_PIXEL_RGBA8 || texture->desc.array_layers!=1 || texture->desc.mip_levels!=1 || !input.width || !input.height ||
            !Range(input.x,input.width,texture->desc.width) || !Range(input.y,input.height,texture->desc.height) ||
            input.first_draw<targetEnd || !Range(input.first_draw,input.draw_count,frame.scene_draw_count)) return fail();
        targetEnd=input.first_draw+input.draw_count;
        for(uint32_t draw=input.first_draw;draw<targetEnd;++draw) if(frame.scene_draws[draw].phase!=RN_SCENE_OFFSCREEN) return fail();
        slot.targets[i].desc=input; slot.targets[i].texture=texture->texture;
    }
    for(size_t i=frame.scene_target_count;i<slot.targets.size();++i) { slot.targets[i].texture.Reset(); slot.targets[i].draws.clear(); }
    uint32_t targetIndex=0;
    if(frame.scene_blob_count) slot.blobs.assign(frame.scene_blobs,frame.scene_blobs+frame.scene_blob_count);
    for(const auto& blob:slot.blobs) if((blob.size && !blob.bytes) || blob.size>(1ull<<30)) return fail();
    while(slot.draws.size()<frame.scene_draw_count) slot.draws.emplace_back();
    for(size_t i=frame.scene_draw_count;i<slot.draws.size();++i) {
        auto& draw=slot.draws[i];
        for(auto& parameters:draw.parameters) if(parameters) parameters->ResetBindingsForReuse();
        draw.textures.clear(); draw.samplers.clear(); draw.textureViews.clear(); draw.textureBindings.clear(); draw.bindings.clear(); draw.texturesBound=false;
        draw.owners={}; draw.geometry={}; draw.texels.clear();
    }
    for(uint32_t i=0;i<frame.scene_draw_count;++i) {
        const auto& input=frame.scene_draws[i]; auto& draw=slot.draws[i];
        if(input.reserved || input.phase>=RN_SCENE_PHASE_COUNT || !Range(input.first_binding,input.binding_count,frame.scene_binding_count) ||
            input.scissor[0]<0 || input.scissor[1]<0 || input.scissor[2]<0 || input.scissor[3]<0 || input.binding_count>64) return fail();
        if(input.phase==RN_SCENE_OFFSCREEN) {
            while(targetIndex<frame.scene_target_count && i>=slot.targets[targetIndex].desc.first_draw+slot.targets[targetIndex].desc.draw_count) ++targetIndex;
            if(targetIndex==frame.scene_target_count || i<slot.targets[targetIndex].desc.first_draw) return fail();
        }
        const radray::array<uint32_t,3> references{input.vertices,input.indices,input.instances};
        radray::array<uint64_t,3> sizes{};
        draw.owners={}; draw.geometry={};
        for(uint32_t index=0;index<3;++index) {
            const auto reference=references[index]; if(reference==UINT32_MAX) continue;
            if(reference&0x80000000u) {
                auto found=_buffers.find(reference&0x7fffffffu); if(found==_buffers.end()) return fail();
                draw.owners[index]=found->second; sizes[index]=found->second->GetDesc().Size;
            } else {
                if(reference>=slot.blobs.size()) return fail(); sizes[index]=slot.blobs[reference].size;
            }
        }
        if(input.vertex_offset>sizes[0] || input.index_offset>sizes[1] || input.instance_offset>sizes[2]) return fail();
        auto* pipeline=GetPipeline(device,input.pipeline,input.phase==RN_SCENE_OFFSCREEN || input.phase==RN_SCENE_GUI); if(!pipeline) return false;
        const auto& desc=pipeline->description;
        const uint32_t oitPhase=input.phase>=RN_SCENE_CLOUD_BOUNDS && input.phase<=RN_SCENE_CLOUD_ACCUMULATE?input.phase-(RN_SCENE_CLOUD_BOUNDS-RN_SCENE_OIT_BOUNDS):input.phase;
        const bool oit=oitPhase>=RN_SCENE_OIT_BOUNDS && oitPhase<=RN_SCENE_OIT_ACCUMULATE;
        const auto format=oitPhase==RN_SCENE_OIT_BOUNDS?TextureFormat::RGBA32_FLOAT:oit?TextureFormat::RGBA16_FLOAT:TextureFormat::RGBA8_UNORM;
        const uint32_t colorCount=input.phase==RN_SCENE_WATER_MASK?0u:oitPhase==RN_SCENE_OIT_TRANSMITTANCE?2u:1u;
        if((oit && !(frame.flags&RN_FRAME_IMPROVED_TRANSPARENCY)) || desc.colors.size()!=colorCount ||
            std::any_of(desc.colors.begin(),desc.colors.end(),[&](const auto& color){return color.Format!=format;})) {
            RADRAY_ERR_LOG("Scene pipeline attachment mismatch: pipeline={} phase={}",input.pipeline,input.phase); return false;
        }
        for(const auto& vertex:desc.vertices) {
            const auto blob=vertex.Binding==0?input.vertices:input.instances;
            const auto count=vertex.StepMode==VertexStepMode::Instance?input.instance_count:input.vertex_count;
            const auto offset=vertex.Binding==0?input.vertex_offset:input.instance_offset;
            if(vertex.Binding>1 || blob==UINT32_MAX || !Range(offset,uint64_t(count)*vertex.ArrayStride,sizes[vertex.Binding==0?0:2])) return fail();
        }
        if(input.indices!=UINT32_MAX && ((input.index_stride!=2 && input.index_stride!=4) ||
            input.index_offset%input.index_stride ||
            !Range(input.index_offset,uint64_t(input.index_count)*input.index_stride,sizes[1]))) return fail();
        if(input.indices==UINT32_MAX && (((desc.topology==1 || desc.topology==5) && input.vertex_count%4) || (desc.topology==3 && input.vertex_count<3))) return fail();
        if(draw.pipeline!=pipeline) {
            for(auto& parameters:draw.parameters) parameters.reset();
            draw.textures.clear(); draw.samplers.clear(); draw.textureViews.clear(); draw.textureBindings.clear(); draw.texels.clear(); draw.texturesBound=false;
            for(uint32_t group=0;group<2;++group) if(pipeline->program->groups[group]) {
                draw.parameters[group]=device.CreateShaderParameterSet({pipeline->program->groups[group].get()}).Release();
                if(!draw.parameters[group]) return false;
            }
            draw.pipeline=pipeline;
        }
        draw.desc=input; draw.bindings.clear();
        if(input.binding_count) draw.bindings.assign(frame.scene_bindings+input.first_binding,frame.scene_bindings+input.first_binding+input.binding_count);
        std::sort(draw.bindings.begin(),draw.bindings.end(),[](const auto& a,const auto& b){return std::pair{a.kind,a.binding}<std::pair{b.kind,b.binding};});
        radray::array<uint64_t,2> present{};
        draw.graphSampler=nullptr;
        for(const auto& binding:draw.bindings) {
            const uint32_t group=binding.kind==0?0:1;
            if(binding.kind>3 || binding.binding>=63 || (present[group]&(1ull<<binding.binding))) return fail();
            if(!(desc.declaredBindings[group]&(1ull<<binding.binding))) return fail();
            present[group]|=1ull<<binding.binding;
            if(binding.kind==0 && (binding.binding==31 || binding.sampler_bits || binding.resource>=slot.blobs.size() || !binding.size ||
                binding.offset%device.GetCapabilities().Limits.CBufferOffsetAlignment || !Range(binding.offset,binding.size,slot.blobs[binding.resource].size))) return fail();
            if(binding.kind==1 && ((binding.binding&1) || binding.offset || binding.size || (binding.sampler_bits&~63u) || !textures.Find(binding.resource))) return fail();
            if(binding.kind==1 && input.phase==RN_SCENE_OFFSCREEN && binding.resource==slot.targets[targetIndex].desc.texture) return fail();
            if(binding.kind==3) {
                auto found=_buffers.find(binding.resource&0x7fffffffu);
                if(!(binding.resource&0x80000000u) || found==_buffers.end() || binding.sampler_bits || !binding.size ||
                    !Range(binding.offset,binding.size,found->second->GetDesc().Size) || !(pipeline->program->texelBindings&(1ull<<binding.binding))) return fail();
            } else if(group==1 && (pipeline->program->texelBindings&(1ull<<binding.binding))) return fail();
            if(binding.kind==2) {
                if(!oit || oitPhase==RN_SCENE_OIT_BOUNDS || (binding.binding&1) || binding.offset || binding.size || binding.sampler_bits ||
                    !binding.resource || binding.resource>3 || (oitPhase==RN_SCENE_OIT_TRANSMITTANCE && binding.resource!=1)) return fail();
                if(!(pipeline->program->activeBindings[1]&(1ull<<binding.binding))) continue;
                auto& sampler=_samplers[1ull<<32];
                if(!sampler) sampler=device.CreateSampler({.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
                    .MinFilter=FilterMode::Nearest,.MagFilter=FilterMode::Nearest,.MipmapFilter=FilterMode::Nearest,.LodMax=0,.AnisotropyClamp=1}).Release();
                if(!sampler) return false;
                draw.graphSampler=sampler.get();
            }
        }
        present[0]|=1ull<<31;
        for(uint32_t group=0;group<2;++group) for(auto binding:pipeline->program->required[group]) if(!(present[group]&(1ull<<binding))) return fail();
        // DXIL and SPIR-V may eliminate different declared blocks in alpha-only variants.
        std::erase_if(draw.bindings,[&](const auto& binding) { return !(pipeline->program->activeBindings[binding.kind==0?0:1]&(1ull<<binding.binding)); });
        // Texture parameter sets remain cached by position and identity across this slot's frames.
        size_t textureIndex=0; bool changed=!draw.texturesBound && bool(draw.parameters[1]);
        for(const auto& binding:draw.bindings) if(binding.kind==1) {
            const auto* texture=textures.Find(binding.resource);
            auto* sampler=GetTextureSampler(device,_samplers,binding.sampler_bits,binding.sampling);
            auto* view=texture->texture->GetSamplingView(device,binding.sampling);
            if(!sampler || !view || !draw.parameters[1]) return fail();
            if(textureIndex>=draw.textures.size()) { draw.textures.push_back({}); draw.samplers.push_back(nullptr); draw.textureViews.push_back(nullptr); draw.textureBindings.push_back(UINT32_MAX); }
            changed|=draw.textures[textureIndex]!=texture->texture || draw.samplers[textureIndex]!=sampler || draw.textureViews[textureIndex]!=view || draw.textureBindings[textureIndex]!=binding.binding;
            ++textureIndex;
        }
        changed|=textureIndex!=draw.textures.size();
        size_t texelIndex=0;
        for(const auto& binding:draw.bindings) if(binding.kind==3) {
            const auto& owner=_buffers.at(binding.resource&0x7fffffffu);
            changed|=texelIndex>=draw.texels.size() || draw.texels[texelIndex].owner!=owner || draw.texels[texelIndex].binding!=binding.binding ||
                draw.texels[texelIndex].range!=BufferRange{binding.offset,binding.size};
            ++texelIndex;
        }
        changed|=texelIndex!=draw.texels.size();
        if(changed) {
            draw.parameters[1]->ResetBindingsForReuse(); draw.textures.clear(); draw.samplers.clear(); draw.textureViews.clear(); draw.textureBindings.clear(); draw.texels.clear();
            for(const auto& binding:draw.bindings) if(binding.kind==1) {
                const auto* texture=textures.Find(binding.resource);
                auto* sampler=GetTextureSampler(device,_samplers,binding.sampler_bits,binding.sampling);
                auto* view=texture->texture->GetSamplingView(device,binding.sampling);
                draw.textures.push_back(texture->texture); draw.samplers.push_back(sampler); draw.textureViews.push_back(view); draw.textureBindings.push_back(binding.binding);
                if(!draw.parameters[1]->Set(binding.binding,0,view)) return false;
                const auto& requiredSamplers=draw.pipeline->program->samplers;
                if(std::find(requiredSamplers.begin(),requiredSamplers.end(),binding.binding+1)!=requiredSamplers.end() && !draw.parameters[1]->Set(binding.binding+1,0,sampler)) return false;
            }
            for(const auto& binding:draw.bindings) if(binding.kind==3) {
                auto& texel=draw.texels.emplace_back(binding.binding,_buffers.at(binding.resource&0x7fffffffu),BufferRange{binding.offset,binding.size},radray::RDGBufferHandle{});
                if(!draw.parameters[1]->Set(binding.binding,0,ShaderTexelBufferBinding{texel.owner->GetBuffer(),texel.range,TextureFormat::R8_SINT})) return false;
            }
            if(!draw.graphSampler && !draw.parameters[1]->FlushWrites()) return false;
            draw.texturesBound=true;
        }
        ++slot.phaseCounts[input.phase];
    }
    slot.guiGroupCount=0; uint32_t previousGui=0;
    if(frame.gui_draw_count && !frame.gui_draws) return fail();
    for(uint32_t i=0;i<frame.gui_draw_count;++i) if(frame.gui_draws[i].pipeline==RN_GUI_SCENE) {
        const uint32_t draw=frame.gui_draws[i].texture;
        if(draw>=frame.scene_draw_count || frame.scene_draws[draw].phase!=RN_SCENE_GUI || (slot.guiGroupCount && draw<=previousGui)) return fail();
        if(slot.guiGroups.size()==slot.guiGroupCount) slot.guiGroups.emplace_back();
        auto& group=slot.guiGroups[slot.guiGroupCount++]; group.desc={}; group.desc.first_draw=draw; group.desc.draw_count=1;
        previousGui=draw;
    }
    if(slot.guiGroupCount!=slot.phaseCounts[RN_SCENE_GUI]) return fail();
    const auto prepareDepth=[&](SceneDraw& target,uint32_t id) {
        if(target.pipeline) return true;
        SceneDraw copy; copy.pipeline=GetPipeline(device,id); if(!copy.pipeline) return false;
        for(uint32_t group=0;group<2;++group) {
            copy.parameters[group]=device.CreateShaderParameterSet({copy.pipeline->program->groups[group].get()}).Release();
            if(!copy.parameters[group]) return false;
        }
        auto& sampler=_samplers[1ull<<32];
        if(!sampler) sampler=device.CreateSampler({.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
            .MinFilter=FilterMode::Nearest,.MagFilter=FilterMode::Nearest,.MipmapFilter=FilterMode::Nearest,.LodMax=0,.AnisotropyClamp=1}).Release();
        if(!sampler) return false; copy.graphSampler=sampler.get(); target=std::move(copy); return true;
    };
    if((frame.flags&RN_FRAME_IMPROVED_TRANSPARENCY) && (slot.phaseCounts[RN_SCENE_WATER_MASK] || slot.phaseCounts[RN_SCENE_CLOUD_BOUNDS]) &&
        !prepareDepth(slot.depthCopy,9)) return false;
    slot.drawCount=frame.scene_draw_count; slot.targetCount=frame.scene_target_count;
    if(!PreparePostEffects(device,slot,frame)) return false;
    slot.consistentDepth=std::any_of(slot.effects.begin(),slot.effects.begin()+slot.effectCount,[](const auto& effect) { return effect.desc.phase==0; });
    if(slot.consistentDepth) {
        if(slot.phaseCounts[RN_SCENE_ALWAYS_ON_TOP] && !prepareDepth(slot.depthIntegrate[0],56)) return false;
        if(slot.phaseCounts[RN_SCENE_HAND] && !prepareDepth(slot.depthIntegrate[1],56)) return false;
    }
    return true;
}
void ScenePassResources::Upload(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame) {
    frame.buffers.clear(); frame.buffers.reserve(frame.blobs.size());
    if(!frame.drawCount && !frame.effectCount) return;
    std::pmr::vector<BufferUses> uses(frame.blobs.size(),&slot.GetScratchMemoryResource());
    for(const auto& draw:std::span{frame.draws}.first(frame.drawCount)) {
        for(auto blob:{draw.desc.vertices,draw.desc.instances}) if(blob!=UINT32_MAX && !(blob&0x80000000u)) uses[blob]|=BufferUse::Vertex;
        if(draw.desc.indices!=UINT32_MAX && !(draw.desc.indices&0x80000000u)) uses[draw.desc.indices]|=BufferUse::Index;
        for(const auto& binding:draw.bindings) if(binding.kind==0) uses[binding.resource]|=BufferUse::CBuffer;
    }
    for(const auto& effect:std::span{frame.effects}.first(frame.effectCount)) if(effect.desc.globals_blob!=UINT32_MAX)
        uses[effect.desc.globals_blob]|=BufferUse::CBuffer;
    for(size_t i=0;i<frame.blobs.size();++i) {
        const auto& blob=frame.blobs[i];
        if(frame.metrics && uses[i]) frame.metrics->scene_upload_bytes+=blob.size;
        frame.buffers.push_back(uses[i]?graph.CreateBuffer({.MaxSize=blob.size,.Usage=uses[i]},
            radray::RDGUploadSource::BorrowedBytes({static_cast<const radray::byte*>(blob.bytes),size_t(blob.size)})):radray::RDGBufferHandle{});
    }
    for(auto& draw:std::span{frame.draws}.first(frame.drawCount)) {
        for(auto& texel:draw.texels) texel.handle=graph.ImportPersistentOwned(texel.owner);
        const radray::array<uint32_t,3> references{draw.desc.vertices,draw.desc.indices,draw.desc.instances};
        for(uint32_t index=0;index<3;++index) if(references[index]!=UINT32_MAX)
            draw.geometry[index]=draw.owners[index]?graph.ImportPersistentOwned(draw.owners[index]):frame.buffers[references[index]];
    }
    const radray::array<float,4> backend{slot.GetDevice().GetBackend()==RenderBackend::Vulkan?-1.f:1.f,0,0,0};
    if(frame.metrics) frame.metrics->scene_upload_bytes+=512;
    frame.backend=graph.CreateBuffer({.MaxSize=512,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(512,backend,
        +[](const radray::array<float,4>& values,radray::RDGUploadWriteContext& context) noexcept {
            std::memset(context.Bytes.data(),0,context.Bytes.size()); std::memcpy(context.Bytes.data(),values.data(),sizeof(values));
            const float screen=-values[0]; std::memcpy(context.Bytes.data()+256,&screen,sizeof(screen)); return true;
        }));
    radray::array<uint32_t,3> counts{};
    for(auto& draw:std::span{frame.draws}.first(frame.drawCount)) if(draw.desc.indices==UINT32_MAX) {
        const auto topology=draw.pipeline->description.topology;
        if(topology==1 || topology==3 || topology==5) {
            draw.desc.index_count=topology==3?(draw.desc.vertex_count-2)*3:draw.desc.vertex_count/4*6;
            const uint32_t type=topology==1?0:topology==3?1:2;
            draw.desc.index_stride=4; counts[type]=std::max(counts[type],draw.desc.index_count);
        }
    }
    frame.generatedIndices={};
    for(uint32_t type=0;type<3;++type) if(counts[type]) {
        const uint64_t size=uint64_t(counts[type])*4;
        if(frame.metrics) frame.metrics->scene_upload_bytes+=size;
        frame.generatedIndices[type]=graph.CreateBuffer({.MaxSize=size,.Usage=BufferUse::Index},radray::RDGUploadSource::Generated(size,
            radray::array<uint32_t,2>{type,counts[type]},+[](const radray::array<uint32_t,2>& values,radray::RDGUploadWriteContext& context) noexcept {
                if(values[0]==0) return WriteQuadIndices(context.Bytes,values[1]/6*4);
                if(values[0]==2) return WriteLineIndices(context.Bytes,values[1]/6*4);
                for(uint32_t i=0;i<values[1];++i) { const uint32_t index=i%3?i/3+i%3:0; std::memcpy(context.Bytes.data()+uint64_t(i)*4,&index,4); }
                return true;
            }));
    }
}
void ScenePassResources::AddPass(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame,rn_scene_phase phase,
    radray::RDGTextureHandle color,radray::RDGTextureHandle depth,uint32_t width,uint32_t height,radray::vector<TextureResource*>& imports,
    radray::RDGTextureHandle second,radray::array<radray::RDGTextureHandle,3> inputs,bool clearDepth,SceneFrameSlot::Target* target) {
    struct Data { SceneFrameSlot* frame; std::span<const SceneFrameSlot::PhaseDraw> draws; uint32_t width,height; radray::array<radray::RDGTextureHandle,3> inputs; };
    auto& draws=target?target->draws:frame.phases[phase]; draws.clear();
    radray::array<bool,3> generated{};
    const uint32_t first=target?target->desc.first_draw:0,last=target?first+target->desc.draw_count:frame.drawCount;
    for(uint32_t i=first;i<last;++i) {
        auto& draw=frame.draws[i]; if(draw.desc.phase!=phase || !draw.desc.instance_count || !draw.desc.vertex_count ||
            (draw.desc.indices!=UINT32_MAX && !draw.desc.index_count)) continue;
        radray::RDGBufferHandle indices;
        const auto topology=draw.pipeline->description.topology;
        if(draw.desc.indices==UINT32_MAX && (topology==1 || topology==3 || topology==5)) {
            const uint32_t type=topology==1?0:topology==3?1:2;
            indices=frame.generatedIndices[type]; generated[type]=true;
        }
        draws.push_back({&draw,indices});
    }
    if(draws.empty()) return;
    auto pass=graph.AddRasterPass("Raynium Scene",Data{&frame,draws,width,height,inputs},+[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
        auto& encoder=context.GetEncoder(); encoder.SetViewport({0,0,float(data.width),float(data.height),0,1});
        return RecordSceneDraws(data.draws,encoder);
    },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
        auto& frame=*data.frame;
        for(auto& item:data.draws) {
            auto& draw=*item.draw; const auto& input=draw.desc;
            const auto fail=[&](std::string_view operation,uint32_t binding=UINT32_MAX) {
                RADRAY_ERR_LOG("Scene Prepare failed: pipeline={} phase={} operation={} binding={}",input.pipeline,input.phase,operation,binding); return false;
            };
            const auto resolve=[&](uint32_t blob,uint64_t offset) {
                auto slice=blob==UINT32_MAX?radray::RDGBufferSlice{}:context.ResolveBuffer(frame.buffers[blob]);
                if(slice.Size) { slice.Offset+=offset; slice.Size-=offset; } return slice;
            };
            const auto geometry=[&](uint32_t index,uint64_t offset) {
                if(!draw.geometry[index].IsValid()) return radray::RDGBufferSlice{};
                auto slice=context.ResolveBuffer(draw.geometry[index]); slice.Offset+=offset; slice.Size-=offset; return slice;
            };
            draw.vertices=geometry(0,input.vertex_offset); draw.instances=geometry(2,input.instance_offset);
            draw.indices=item.generated.IsValid()?context.ResolveBuffer(item.generated):geometry(1,input.index_offset);
            for(const auto& vertex:draw.pipeline->description.vertices) {
                auto& slice=vertex.Binding==0?draw.vertices:draw.instances;
                slice.Size=uint64_t(vertex.ArrayStride)*(vertex.StepMode==VertexStepMode::Instance?input.instance_count:input.vertex_count);
            }
            if(draw.indices.Size) draw.indices.Size=uint64_t(input.index_count)*input.index_stride;
            if(draw.indices.Offset>UINT32_MAX) return fail("index offset");
            auto& parameters=*draw.parameters[0]; parameters.ResetBindingsForReuse();
            struct Cleanup { ShaderParameterSet* parameters; ~Cleanup() noexcept { parameters->ResetBindingsForReuse(); } };
            context.GetSlot().Emplace<Cleanup>(&parameters);
            auto backend=context.ResolveBuffer(frame.backend); backend.Size=256;
            if(draw.pipeline->description.screen) backend.Offset+=256;
            if(!parameters.Set(31,0,ShaderBufferBinding{backend.Buffer.Get(),{backend.Offset,backend.Size}})) return fail("backend constant",31);
            for(const auto& binding:draw.bindings) if(binding.kind==0) {
                const auto slice=resolve(binding.resource,binding.offset);
                if(!parameters.Set(binding.binding,0,ShaderBufferBinding{slice.Buffer.Get(),{slice.Offset,binding.size}})) return fail("constant",binding.binding);
            }
            if(!parameters.FlushWrites()) return fail("constant flush");
            if(draw.graphSampler) {
                auto& textures=*draw.parameters[1];
                struct TextureCleanup { SceneDraw* draw; ~TextureCleanup() noexcept { draw->parameters[1]->ResetBindingsForReuse(); draw->texturesBound=false; } };
                context.GetSlot().Emplace<TextureCleanup>(&draw);
                for(const auto& binding:draw.bindings) if(binding.kind==2) {
                    if(!data.inputs[binding.resource-1].IsValid()) return fail("graph input",binding.binding);
                    auto view=context.ResolveView(data.inputs[binding.resource-1],TextureViewUsage::Resource);
                    if(!view || !textures.Set(binding.binding,0,view.Get())) return fail("graph texture",binding.binding);
                    const auto& samplers=draw.pipeline->program->samplers;
                    if(std::find(samplers.begin(),samplers.end(),binding.binding+1)!=samplers.end() && !textures.Set(binding.binding+1,0,draw.graphSampler)) return fail("graph sampler",binding.binding+1);
                }
                if(!textures.FlushWrites()) return fail("texture flush");
            }
            if(frame.metrics) {
                ++frame.metrics->scene_calls;
                frame.metrics->bind_calls+=1+uint64_t(bool(draw.parameters[0]))+uint64_t(bool(draw.parameters[1]))
                    +uint64_t(draw.vertices.Size!=0)+uint64_t(draw.indices.Size!=0)+uint64_t(draw.instances.Size!=0);
            }
        }
        return true;
    });
    const bool readDepth=(phase>=RN_SCENE_OIT_BOUNDS && phase<=RN_SCENE_OIT_ACCUMULATE) || (phase>RN_SCENE_CLOUD_BOUNDS && phase<=RN_SCENE_CLOUD_ACCUMULATE);
    if(phase!=RN_SCENE_WATER_MASK) pass.WriteColor(0,color,LoadAction::Load,StoreAction::Store);
    pass.SetDepthStencil(depth,readDepth?TextureState::DepthRead:TextureState::DepthWrite,
        clearDepth?LoadAction::Clear:LoadAction::Load,StoreAction::Store,LoadAction::DontCare,StoreAction::Discard,{0,0});
    if(second.IsValid()) pass.WriteColor(1,second,LoadAction::Load,StoreAction::Store);
    pass.ReadBuffer(frame.backend,BufferState::CBuffer);
    for(uint32_t type=0;type<3;++type) if(generated[type]) pass.ReadBuffer(frame.generatedIndices[type],BufferState::Index);
    for(auto input:inputs) if(input.IsValid()) pass.ReadTexture(input);
    std::pmr::unordered_set<TextureResource*> unique{&slot.GetScratchMemoryResource()};
    // Declarations follow only the actual draws in this phase.
    for(const auto& item:draws) {
        const auto& draw=*item.draw;
        if(draw.geometry[0].IsValid()) pass.ReadBuffer(draw.geometry[0],BufferState::Vertex);
        if(draw.geometry[2].IsValid()) pass.ReadBuffer(draw.geometry[2],BufferState::Vertex);
        if(draw.geometry[1].IsValid()) pass.ReadBuffer(draw.geometry[1],BufferState::Index);
        for(const auto& binding:draw.bindings) if(binding.kind==0) pass.ReadBuffer(frame.buffers[binding.resource],BufferState::CBuffer);
        for(const auto& texture:draw.textures) if(unique.insert(texture.Get()).second) pass.ReadTexture(texture->Import(graph,slot,imports));
        for(const auto& texel:draw.texels) pass.ReadBuffer(texel.handle,BufferState::ShaderRead);
    }
}
void ScenePassResources::AddOffscreenPasses(radray::RDGBuilder& graph,radray::FrameSlot& slot,SceneFrameSlot& frame,radray::vector<TextureResource*>& imports) {
    for(auto& target:std::span{frame.targets}.first(frame.targetCount)) {
        const auto& tile=target.desc;
        const auto color=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=tile.width,.Height=tile.height,.DepthOrArraySize=1,
            .MipLevels=1,.SampleCount=1,.Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::CopySource});
        const auto depth=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=tile.width,.Height=tile.height,.DepthOrArraySize=1,
            .MipLevels=1,.SampleCount=1,.Format=TextureFormat::D32_FLOAT,.Memory=MemoryType::Device,.Usage=TextureUse::DepthStencilWrite|TextureUse::DepthStencilRead});
        struct Clear {};
        graph.AddRasterPass("Raynium Offscreen Clear",Clear{},+[](const Clear&,radray::RDGRasterPassContext&) noexcept { return true; },
            +[](Clear&,radray::RDGPrepareContext&) noexcept { return true; })
            .WriteColor(0,color,LoadAction::Clear,StoreAction::Store,{{0,0,0,0}})
            .SetDepthStencil(depth,TextureState::DepthWrite,LoadAction::Clear,StoreAction::Store,LoadAction::DontCare,StoreAction::Discard,{0,0});
        AddPass(graph,slot,frame,RN_SCENE_OFFSCREEN,color,depth,tile.width,tile.height,imports,{},{},false,&target);
        const auto destination=target.texture->Import(graph,slot,imports);
        struct Copy { radray::RDGTextureHandle source,target; rn_scene_target tile; TextureCopyDescriptor copy{}; };
        graph.AddCopyPass("Raynium Offscreen Cache",Copy{color,destination,tile},+[](const Copy& data,radray::RDGCopyPassContext& context) noexcept {
            context.GetCommandBuffer().CopyTextureToTexture(data.copy); return true;
        },+[](Copy& data,radray::RDGPrepareContext& context) noexcept {
            data.copy={.Destination=&context.Resolve(data.target),.DestinationX=data.tile.x,.DestinationY=data.tile.y,.Source=&context.Resolve(data.source),
                .Width=data.tile.width,.Height=data.tile.height}; return true;
        }).ReadTexture(color).WriteTexture(destination);
        graph.MarkOutput(destination);
    }
}
void ScenePassResources::AddDepthCopy(radray::RDGBuilder& graph,SceneFrameSlot& frame,radray::RDGTextureHandle source,
    radray::RDGTextureHandle target,uint32_t width,uint32_t height,int integrate) {
    struct Data { SceneDraw* draw; radray::RDGBufferHandle backend; radray::RDGTextureHandle source; uint32_t width,height; rn_frame_metrics* metrics; };
    graph.AddRasterPass(integrate<0?"Raynium Depth Blit":"Raynium Depth Integrate",
        Data{integrate<0?&frame.depthCopy:&frame.depthIntegrate[size_t(integrate)],frame.backend,source,width,height,frame.metrics},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            return RecordSceneScreen(*data.draw,data.width,data.height,context.GetEncoder());
        },+[](Data& data,radray::RDGPrepareContext& context) noexcept {
            auto& draw=*data.draw;
            struct Cleanup { SceneDraw* draw; ~Cleanup() noexcept { for(auto& group:draw->parameters) group->ResetBindingsForReuse(); } };
            context.GetSlot().Emplace<Cleanup>(&draw);
            for(auto& group:draw.parameters) group->ResetBindingsForReuse();
            auto slice=context.ResolveBuffer(data.backend);
            auto view=context.ResolveView(data.source,TextureViewUsage::Resource);
            if(!view || !draw.parameters[0]->Set(31,0,ShaderBufferBinding{slice.Buffer.Get(),{slice.Offset+256,256}}) || !draw.parameters[0]->FlushWrites()
                || !draw.parameters[1]->Set(0,0,view.Get())) return false;
            const auto& samplers=draw.pipeline->program->samplers;
            if(std::find(samplers.begin(),samplers.end(),1u)!=samplers.end() && !draw.parameters[1]->Set(1,0,draw.graphSampler)) return false;
            if(!draw.parameters[1]->FlushWrites()) return false;
            if(data.metrics) { ++data.metrics->scene_calls; data.metrics->bind_calls+=3; }
            return true;
        }).ReadBuffer(frame.backend,BufferState::CBuffer).ReadTexture(source)
        .SetDepthStencil(target,TextureState::DepthWrite,integrate<0?LoadAction::DontCare:LoadAction::Load,StoreAction::Store);
}
#include "scene_post.inc"

}
