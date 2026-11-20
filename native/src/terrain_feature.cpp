#include <raynium/terrain_feature.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;

void TerrainView::Import(radray::RDGBuilder& graph,GpuBackend backend) {
    for(auto& upload:uploads) {
        auto h=graph.ImportPersistentOwned(upload.target);
        graph.UploadBuffer(h,upload.offset,std::move(upload.source)); graph.MarkOutput(h);
    }
    uploads.clear();
    if(lists.batches.empty()) return;
    struct Source { const rn_terrain_view_desc* parameters; GpuBackend backend; };
    constants=graph.CreateBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,Source{&frame.parameters,backend},
        +[](const Source& source,radray::RDGUploadWriteContext& ctx) noexcept {
            struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4]; } data{};
            const auto& p=*source.parameters;
            std::copy_n(p.projection,16,data.projection); std::copy_n(p.view,16,data.view);
            std::copy_n(p.camera_block,3,data.block); data.block[3]=(p.flags&RN_FRAME_RGSS)!=0;
            std::copy_n(p.camera_fraction,3,data.fraction);
            data.fraction[3]=source.backend==GpuBackend::Vulkan?1.f:-1.f;
            std::copy_n(p.fog_color,4,data.fog); std::copy_n(p.fog_distances,4,data.distances);
            std::memset(ctx.Bytes.data(),0,ctx.Bytes.size()); std::memcpy(ctx.Bytes.data(),&data,sizeof(data)); return true;
        }));
    auto bytes=std::as_bytes(frame.instances);
    instances=graph.CreateBuffer({.MaxSize=bytes.size(),.Usage=BufferUse::Vertex},radray::RDGUploadSource::BorrowedBytes(bytes));
    if(useIndirect) {
        auto args=std::as_bytes(std::span{lists.arguments});
        indirect=graph.CreateBuffer({.MaxSize=args.size(),.Usage=BufferUse::Indirect},radray::RDGUploadSource::BorrowedBytes(args));
    }
}

bool TerrainFeature::Decode(std::span<const radray::byte> bytes,std::span<TerrainView> views,const TextureRegistry& textures,const radray::Rect& area) {
    struct Material { uint32_t id,flags; rn_texture_sampling sampling; };
    struct Wire { uint32_t opcode,size,view,stage; radray::Rect scissor; uint32_t ids[3],mips[3]; Material materials[2]; } w{};
    if(bytes.size()!=sizeof(w)) return false;
    std::memcpy(&w,bytes.data(),sizeof(w));
    if(w.view>=views.size() || w.stage>OitAccumulate || w.scissor.X<area.X || w.scissor.Y<area.Y ||
       uint64_t(w.scissor.X)+w.scissor.Width>uint64_t(area.X)+area.Width || uint64_t(w.scissor.Y)+w.scissor.Height>uint64_t(area.Y)+area.Height) return false;
    view=&views[w.view]; stage=w.stage; scissor=w.scissor;
    for(uint32_t i=0;i<2;++i) {
        const auto& m=w.materials[i];
        const auto* image=textures.Find(m.id);
        if(!image || image->desc.array_layers!=1 || IsDepthStencilFormat(image->texture->GetDesc().Format) ||
           !m.sampling.mip_count || uint64_t(m.sampling.base_mip)+m.sampling.mip_count>image->desc.mip_levels) return false;
        materials[i]={image->texture,m.sampling,m.flags,{}};
    }
    uint32_t required=stage==OitTransmittance?1:stage==OitAccumulate?3:0;
    for(uint32_t i=0;i<required;++i) {
        auto* image=textures.Find(w.ids[i]);
        if(!image || w.mips[i]>=image->desc.mip_levels || image->desc.array_layers!=1) return false;
        sources[i]=image->texture; mips[i]=w.mips[i];
    }
    return true;
}
void TerrainFeature::Import(radray::RDGBuilder& graph,FrameTextureImports& imports) {
    const auto layer=std::min(stage,uint32_t(Translucent));
    const auto& list=view->lists.layers[layer];
    record={&view->lists,layer,view->indirectLimit,view->useIndirect};
    radray::unordered_set<radray::GpuBuffer*> vb,ib;
    for(auto& m:materials) m.handle=imports.Import(m.image);
    for(const auto& batch:std::span{view->lists.batches}.subspan(list.firstBatch,list.batchCount)) {
        if(vb.insert(batch.vertexPage).second) vertexPages.push_back(graph.ImportPersistentBorrowed(*batch.vertexPage));
        if(ib.insert(batch.indexPage).second) indexPages.push_back(graph.ImportPersistentBorrowed(*batch.indexPage));
    }
    for(uint32_t i=0;i<3;++i) if(sources[i]) reads[i]=imports.Import(sources[i]);
}
void TerrainFeature::Declare(radray::RDGRasterPassBuilder& pass) const {
    if(!view->lists.layers[record.layer].recordCount) return;
    pass.ReadBuffer(view->constants,BufferState::CBuffer).ReadBuffer(view->instances,BufferState::Vertex);
    if(view->useIndirect) pass.ReadBuffer(view->indirect,BufferState::Indirect);
    for(auto h:vertexPages) pass.ReadBuffer(h,BufferState::Vertex);
    for(auto h:indexPages) pass.ReadBuffer(h,BufferState::Index);
    for(const auto& m:materials) pass.ReadTexture(m.handle);
    for(auto h:reads) if(h.IsValid()) pass.ReadTexture(h);
}
bool TerrainFeature::Prepare(radray::RDGPrepareContext& ctx) {
    if(!view->lists.layers[record.layer].recordCount) return true;
    auto& slot=ctx.GetSlot();
    pipeline=view->mesh->Pipeline(stage,(view->frame.parameters.flags&RN_FRAME_WIREFRAME)!=0);
    if(!pipeline) return false;
    record.instances=ctx.ResolveBuffer(view->instances);
    if(view->useIndirect) record.indirect=ctx.ResolveBuffer(view->indirect);
    auto binding=FrameConstantBinding(0,ctx.ResolveBuffer(view->constants));
    constants=slot.AcquireParameterSet(view->mesh->PassGroupLayout(),{&binding,1}).Get();
    if(!constants) return false;
    radray::array<radray::TransientParameterBinding,4> materialBindings;
    for(uint32_t i=0;i<2;++i) {
        const auto& m=materials[i];
        auto* srv=GetSamplingView(slot.GetDevice(),*m.image,m.sampling);
        auto* sampler=GetTextureSampler(slot.GetDevice(),m.flags,m.sampling);
        if(!srv || !sampler) return false;
        materialBindings[i]={1+i,0,srv};
        materialBindings[i+2]={3+i,0,sampler};
    }
    record.material=slot.AcquireParameterSet(view->mesh->MaterialGroupLayout(),materialBindings).Get();
    if(!record.material) return false;
    if(sources[0]) {
        radray::array<radray::TransientParameterBinding,4> bindings;
        for(uint32_t i=0;i<4;++i) {
            uint32_t j=i<3 && sources[i]?i:0;
            auto* srv=GetSamplingView(slot.GetDevice(),*sources[j],{mips[j],1,0,1});
            if(!srv) return false;
            bindings[i]={i,0,srv};
        }
        oit=slot.AcquireParameterSet(view->mesh->OitGroupLayout(),bindings).Get();
        if(!oit) return false;
    }
    return true;
}
bool TerrainFeature::Record(GraphicsCommandEncoder& enc) const {
    if(!pipeline) return true;
    enc.SetScissor(scissor);
    enc.BindGraphicsPipelineState(pipeline);
    enc.BindShaderParameterSet(0,constants);
    if(oit) enc.BindShaderParameterSet(2,oit);
    return RecordTerrainDraws(record,enc);
}
}
