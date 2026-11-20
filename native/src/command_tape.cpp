#include <raynium/command_tape.h>
#include <radray/logger.h>
#include <raynium/output_pass.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <numeric>

namespace raynium {
namespace {
using namespace radray::gpu;

enum class Opcode : uint32_t { ClearColor=1, ClearDepth, CaptureColor, UploadTexture, CopyTexture, BlitToSurface, WriteTimestamp, WriteBufferBytes, CopyBufferBytes, BufferToTexture, TextureToBuffer, BeginPass, EndPass, Draw, RetainedTerrain };
struct Header { Opcode opcode; uint32_t size; };
struct WireRegion { uint32_t id,mip,layer,x,y,width,height; };
struct WireClear { Header header; WireRegion target; float value[4]; uint32_t reserved; };
struct WireCapture { Header header; WireRegion source; uint32_t reserved; uint64_t id; };
struct WireUpload { Header header; WireRegion target; uint32_t pitch; uint64_t offset; };
struct WireBlit { Header header; uint32_t texture,mip; };
struct WireCopy { Header header; WireRegion source,destination; };
static_assert(sizeof(WireClear)==56 && sizeof(WireCapture)==48 && sizeof(WireUpload)==48 && sizeof(WireCopy)==64);

template<class T>
bool Read(std::span<const radray::byte> bytes,T& record) {
    if(bytes.size()!=sizeof(T)) return false;
    std::memcpy(&record,bytes.data(),sizeof(T));
    return true;
}
void CopyBytes(radray::RDGBuilder& graph,radray::RDGBufferHandle source,uint64_t srcOffset,radray::RDGBufferHandle destination,
               uint64_t dstOffset,uint64_t size) {
    struct Pass { radray::RDGBufferHandle src,dst; uint64_t srcOffset,dstOffset,size; };
    graph.AddCopyPass("Raynium buffer copy",Pass{source,destination,srcOffset,dstOffset,size},
        +[](const Pass& pass,radray::RDGCopyPassContext& ctx) noexcept {
            const auto from=ctx.ResolveBuffer(pass.src),to=ctx.ResolveBuffer(pass.dst);
            ctx.GetCommandBuffer().CopyBufferToBuffer(to.Buffer.Get(),to.Offset+pass.dstOffset,from.Buffer.Get(),from.Offset+pass.srcOffset,pass.size);
            return true;
        }).ReadBuffer(source).WriteBuffer(destination);
}
void CopyRegion(radray::RDGBuilder& graph,radray::RDGTextureHandle source,const TextureRegion2D& from,
                radray::RDGTextureHandle destination,const TextureRegion2D& to) {
    struct Pass {
        radray::RDGTextureHandle source,destination;
        TextureCopyDescriptor copy;
    };
    graph.AddCopyPass("Raynium texture copy",Pass{source,destination,{
        .DestinationMipLevel=to.MipLevel,.DestinationArrayLayer=to.ArrayLayer,.DestinationX=to.X,.DestinationY=to.Y,
        .SourceMipLevel=from.MipLevel,.SourceArrayLayer=from.ArrayLayer,.SourceX=from.X,.SourceY=from.Y,
        .Width=from.Width,.Height=from.Height}},
        +[](const Pass& pass,radray::RDGCopyPassContext& context) noexcept {
            context.GetCommandBuffer().CopyTextureToTexture(pass.copy);
            return context.GetCommandBuffer().IsValid();
        },+[](Pass& pass,radray::RDGPrepareContext& context) noexcept {
            pass.copy.Source=&context.Resolve(pass.source);
            pass.copy.Destination=&context.Resolve(pass.destination);
            return true;
        }).ReadTexture(source).WriteTexture(destination);
}
}

void CommandTape::Measure(rn_frame_metrics& metrics) const {
    for(const auto& view:_views)
        for(const auto& geometry:view.uploads) metrics.terrain_upload_bytes+=geometry.source.GetMaxSize();
    for(const auto& command:_commands) {
        if(const auto* raster=std::get_if<radray::shared_ptr<ImmediatePass>>(&command)) (*raster)->Measure(metrics);
        else if(const auto* upload=std::get_if<Upload>(&command)) metrics.texture_upload_bytes+=upload->pixels.size();
        else if(const auto* write=std::get_if<WriteBuffer>(&command)) metrics.scene_upload_bytes+=write->bytes.size();
    }
}
bool CommandTape::PrepareViews(const rn_batch_desc& batch,TerrainResidency& terrain,const TextureRegistry& textures,const MeshPassResources& mesh,
    radray::FrameSlot& slot,bool indirect) {
    if(batch.struct_size!=sizeof(batch) || batch.view_reserved || (batch.view_count && !batch.views) || batch.view_count>65536) return false;
    _views.resize(batch.view_count);
    _backend=slot.GetDevice().GetBackend();
    for(size_t i=0;i<_views.size();++i) {
        auto& view=_views[i];
        view.mesh=&mesh; view.useIndirect=indirect;
        view.indirectLimit=slot.GetDevice().GetCapabilities().Limits.MaxDrawIndirectCount;
        if(!terrain.PrepareFrame(textures,batch.views[i],mesh,{slot,view.uploads,view.lists,indirect},view.frame)) return false;
    }
    return true;
}
bool CommandTape::Decode(const rn_batch_desc& batch,const TextureRegistry& textures,const QueryRegistry& queries,const BufferRegistry& buffers,const ImmediatePipelineRegistry& pipelines,uint32_t maxColors) {
    if(batch.struct_size!=sizeof(batch) || (batch.flags&~RN_BATCH_MEASURE) || batch.reserved || (batch.dirty_count && !batch.dirty_ranges) ||
       (batch.command_bytes && !batch.commands) || (batch.upload_bytes && !batch.uploads) ||
       batch.command_bytes>PTRDIFF_MAX || batch.upload_bytes>PTRDIFF_MAX) {
        RADRAY_ERR_LOG("Invalid command batch descriptor");
        return false;
    }
    const auto resolve=[&](const WireRegion& wire,Region& result) {
        auto* entry=textures.Find(wire.id);
        if(!entry || wire.mip>=entry->desc.mip_levels || wire.layer>=entry->desc.array_layers) return false;
        const uint32_t width=std::max(1u,entry->desc.width>>wire.mip),height=std::max(1u,entry->desc.height>>wire.mip);
        if(!wire.width || !wire.height || uint64_t(wire.x)+wire.width>width || uint64_t(wire.y)+wire.height>height) return false;
        result={entry->texture,{wire.mip,wire.layer,wire.x,wire.y,wire.width,wire.height},wire.width==width && wire.height==height};
        return true;
    };
    std::span commands{static_cast<const radray::byte*>(batch.commands),size_t(batch.command_bytes)};
    const std::span uploads{static_cast<const radray::byte*>(batch.uploads),size_t(batch.upload_bytes)};
    bool hasBlit=false;
    radray::shared_ptr<ImmediatePass> raster;
    while(!commands.empty()) {
        Header header{};
        if(commands.size()<sizeof(header)) return false;
        std::memcpy(&header,commands.data(),sizeof(header));
        if(header.size<sizeof(header) || header.size>commands.size()) return false;
        const auto bytes=commands.first(header.size);
        commands=commands.subspan(header.size);
        if(raster && header.opcode!=Opcode::RetainedTerrain && header.opcode!=Opcode::Draw && header.opcode!=Opcode::WriteTimestamp && header.opcode!=Opcode::EndPass) return false;
        Region region{};
        switch(header.opcode) {
        case Opcode::ClearColor:
        case Opcode::ClearDepth: {
            WireClear wire{};
            if(!Read(bytes,wire) || wire.reserved || !resolve(wire.target,region)) return false;
            const auto& desc=region.texture->GetDesc();
            bool depth=header.opcode==Opcode::ClearDepth;
            if(depth!=IsDepthStencilFormat(desc.Format) || !(desc.Usage&(depth?TextureUse::DepthStencilWrite:TextureUse::RenderTarget))) return false;
            if(depth && !(wire.value[0]>=0 && wire.value[0]<=1)) return false;
            ColorClearValue value{};
            std::copy_n(wire.value,4,value.Value.begin());
            _commands.emplace_back(Clear{region,value});
            break;
        }
        case Opcode::CaptureColor: {
            WireCapture wire{};
            if(!Read(bytes,wire) || wire.reserved || !wire.id || !resolve(wire.source,region)) return false;
            const auto& desc=region.texture->GetDesc();
            if(desc.Format!=TextureFormat::RGBA8_UNORM || !(desc.Usage&TextureUse::CopySource)) return false;
            _commands.emplace_back(Capture{region,wire.id});
            break;
        }
        case Opcode::UploadTexture: {
            WireUpload wire{};
            if(!Read(bytes,wire) || !resolve(wire.target,region)) return false;
            const auto format=region.texture->GetDesc().Format;
            if(IsDepthStencilFormat(format)) return false;
            const uint64_t row=uint64_t(wire.target.width)*GetTextureFormatBytesPerPixel(format);
            const uint64_t size=uint64_t(wire.pitch)*(wire.target.height-1)+row;
            if(wire.pitch<row || wire.offset>uploads.size() || size>uploads.size()-wire.offset) return false;
            auto pixels=uploads.subspan(wire.offset,size);
            _commands.emplace_back(Upload{region,{pixels.begin(),pixels.end()},wire.pitch});
            break;
        }
        case Opcode::CopyTexture: {
            WireCopy wire{};
            Region destination{};
            if(!Read(bytes,wire) || !resolve(wire.source,region) || !resolve(wire.destination,destination)) return false;
            const auto& src=region.texture->GetDesc();
            if(!(src.Usage&TextureUse::CopySource) || src.Format!=destination.texture->GetDesc().Format ||
                wire.source.width!=wire.destination.width || wire.source.height!=wire.destination.height ||
                (GetTextureFormatAspects(src.Format)&TextureAspect::Stencil) || (IsDepthStencilFormat(src.Format) && !(region.full && destination.full))) return false;
            const auto& a=wire.source;
            const auto& b=wire.destination;
            if(a.id==b.id && a.mip==b.mip && a.layer==b.layer && a.x<b.x+b.width && b.x<a.x+a.width &&
               a.y<b.y+b.height && b.y<a.y+a.height) return false;
            _commands.emplace_back(Copy{region,destination});
            break;
        }
        case Opcode::BlitToSurface: {
            WireBlit wire{};
            if(!Read(bytes,wire) || !batch.surface || hasBlit) return false;
            auto* entry=textures.Find(wire.texture);
            if(!entry || wire.mip>=entry->desc.mip_levels || entry->desc.array_layers!=1 ||
               !(entry->desc.flags&RN_TEXTURE_READBACK) || IsDepthStencilFormat(entry->texture->GetDesc().Format) ||
               IsUintFormat(entry->texture->GetDesc().Format) || IsSintFormat(entry->texture->GetDesc().Format)) return false;
            hasBlit=true;
            _commands.emplace_back(Blit{{entry->texture,{wire.mip,0,0,0,std::max(1u,entry->desc.width>>wire.mip),
                std::max(1u,entry->desc.height>>wire.mip)},true}});
            break;
        }
        case Opcode::WriteTimestamp: {
            struct WireTimestamp { Header header; uint32_t pool,slot; } wire{};
            if(!Read(bytes,wire) || _timestamps.size()==UINT32_MAX) return false;
            auto pool=queries.Find(wire.pool);
            if(!pool || wire.slot>=pool->slots.size()) return false;
            if(raster) raster->WriteTimestamp(uint32_t(_timestamps.size()));
            else _commands.emplace_back(Timestamp{uint32_t(_timestamps.size())});
            _timestamps.push_back({std::move(pool),wire.slot});
            break;
        }
        case Opcode::WriteBufferBytes: {
            struct Wire { Header header; uint32_t id,reserved; uint64_t offset,size,uploadOffset; } wire{};
            if(!Read(bytes,wire) || wire.reserved) return false;
            auto target=buffers.Find(wire.id);
            if(!target || !(target->usage&RN_BUFFER_COPY_DST) || !target->Contains(wire.offset,wire.size) ||
               wire.uploadOffset>uploads.size() || wire.size>uploads.size()-wire.uploadOffset) return false;
            auto pixels=uploads.subspan(wire.uploadOffset,wire.size);
            _commands.emplace_back(WriteBuffer{std::move(target),wire.offset,{pixels.begin(),pixels.end()}});
            break;
        }
        case Opcode::CopyBufferBytes: {
            struct Wire { Header header; uint32_t source,destination; uint64_t srcOffset,dstOffset,size; } wire{};
            if(!Read(bytes,wire)) return false;
            auto src=buffers.Find(wire.source),dst=buffers.Find(wire.destination);
            if(!src || !dst || !(src->usage&RN_BUFFER_COPY_SRC) || !(dst->usage&RN_BUFFER_COPY_DST) ||
               !src->Contains(wire.srcOffset,wire.size) || !dst->Contains(wire.dstOffset,wire.size) ||
               (src==dst && wire.srcOffset<wire.dstOffset+wire.size && wire.dstOffset<wire.srcOffset+wire.size)) return false;
            _commands.emplace_back(CopyBuffer{std::move(src),std::move(dst),wire.srcOffset,wire.dstOffset,wire.size,0});
            break;
        }
        case Opcode::BufferToTexture:
        case Opcode::TextureToBuffer: {
            struct Wire { Header header; WireRegion image; uint32_t buffer; uint64_t offset,pitch; } wire{};
            if(!Read(bytes,wire) || !resolve(wire.image,region)) return false;
            const bool toImage=header.opcode==Opcode::BufferToTexture;
            auto buffer=buffers.Find(wire.buffer);
            const auto& image=region.texture->GetDesc();
            const uint64_t row=uint64_t(region.area.Width)*GetTextureFormatBytesPerPixel(image.Format);
            if(!buffer || !(buffer->usage&(toImage?RN_BUFFER_COPY_SRC:RN_BUFFER_COPY_DST)) ||
               (!toImage && !(image.Usage&TextureUse::CopySource)) || wire.pitch>UINT32_MAX || wire.pitch<row ||
               (!toImage && wire.pitch!=row) || !buffer->Contains(wire.offset,wire.pitch*(region.area.Height-1)+row) ||
               (IsDepthStencilFormat(image.Format) && (toImage || !region.full || (GetTextureFormatAspects(image.Format)&TextureAspect::Stencil)))) return false;
            _commands.emplace_back(TextureBuffer{region,std::move(buffer),wire.offset,wire.pitch,0,0,toImage});
            break;
        }
        case Opcode::RetainedTerrain:
            if(!raster || !raster->AddTerrain(bytes,_views,textures)) return false;
            break;
        case Opcode::Draw:
            if(!raster || !raster->AddDraw(bytes,pipelines,buffers,textures)) return false;
            break;
        case Opcode::BeginPass:
            raster=ImmediatePass::Decode(bytes,textures,maxColors);
            if(!raster) return false;
            _commands.emplace_back(raster);
            break;
        case Opcode::EndPass:
            if(!raster || bytes.size()!=sizeof(Header)) return false;
            raster.reset();
            break;
        default: return false;
        }
    }
    return !raster && hasBlit==(batch.surface!=nullptr);
}

bool CommandTape::PrepareReadbacks(Device& device,FrameReadbacks& frame) {
    for(auto& command:_commands) {
        if(auto* image=std::get_if<TextureBuffer>(&command)) {
            auto& desc=image->image.texture->GetDesc();
            const auto& area=image->image.area;
            uint64_t minimum=IsDepthStencilFormat(desc.Format)?4u:1u;
            const uint64_t texel=GetTextureFormatBytesPerPixel(desc.Format);
            const uint64_t row=texel*area.Width;
            const uint64_t alignment=std::lcm(texel,std::max<uint64_t>(minimum,device.GetDetail().TextureDataPitchAlignment));
            const uint64_t placement=std::lcm(texel,std::max<uint64_t>(minimum,device.GetDetail().TextureDataPlacementAlignment));
            const uint64_t pitch=(row+alignment-1)/alignment*alignment;
            if(pitch>UINT32_MAX) return false;
            image->alignedPitch=uint32_t(pitch);
            image->direct=image->offset%placement==0 && image->pitch%alignment==0 &&
                image->buffer->Contains(image->offset,image->pitch*area.Height);
            if(!image->buffer->host.empty() && !frame.Reserve(image->buffer,image->offset,row,area.Height,pitch,placement,image->stagingOffset))
                return false;
        }
        if(auto* write=std::get_if<WriteBuffer>(&command); write && !write->target->host.empty())
            frame.AddInline(write->target,write->offset,std::move(write->bytes));
        if(auto* copy=std::get_if<CopyBuffer>(&command); copy && !copy->destination->host.empty()) {
            if(!frame.Reserve(copy->destination,copy->dstOffset,copy->size,1,copy->size,
                              std::max<uint64_t>(1,device.GetDetail().BufferCopyOffsetAlignment),copy->stagingOffset)) return false;
        }
    }
    return frame.Prepare(device);
}
bool CommandTape::ReserveReadbacks(radray::AsyncReadbackQueue& queue) const {
    for(const auto& command:_commands) {
        if(const auto* read=std::get_if<Capture>(&command)) {
            const auto& region=read->source.area;
            if(!queue.ReserveTexture(read->id,{region.Width,region.Height,read->source.texture->GetDesc().Format})) return false;
        }
    }
    return true;
}

void CommandTape::ReplayTextureBuffer(radray::RDGBuilder& graph,FrameTextureImports& imports,const FrameReadbacks& readbacks,const TextureBuffer& op) {
    auto texture=imports.Import(op.image.texture,op.toImage && op.image.full);
    const auto& region=op.image.area;
    const uint64_t row=uint64_t(region.Width)*GetTextureFormatBytesPerPixel(op.image.texture->GetDesc().Format);
    const bool host=!op.buffer->host.empty();
    if(op.toImage && !op.direct && op.buffer->upload) {
        radray::vector<radray::byte> packed(row*region.Height);
        for(uint32_t y=0;y<region.Height;++y)
            std::memcpy(packed.data()+y*row,op.buffer->mapped+op.offset+y*op.pitch,row);
        graph.UploadTexture(texture,region,radray::RDGTextureUploadSource::OwnedBytes(std::move(packed),row));
        graph.MarkOutput(texture);
        return;
    }
    auto real=host?readbacks.Import(graph):op.buffer->Import(graph);
    const bool intermediate=!host && !op.direct;
    auto buffer=intermediate?graph.CreateBuffer({.Size=uint64_t(op.alignedPitch)*region.Height,.Memory=MemoryType::Device,
        .Usage=BufferUse::CopySource|BufferUse::CopyDestination}):real;
    const uint64_t offset=host?op.stagingOffset:intermediate?0:op.offset;
    const uint32_t pitch=host || intermediate?op.alignedPitch:uint32_t(op.pitch);
    if(op.toImage && intermediate) {
        for(uint32_t y=0;y<region.Height;++y) CopyBytes(graph,real,op.offset+y*op.pitch,buffer,uint64_t(y)*pitch,row);
    }
    struct Transfer {
        radray::RDGTextureHandle texture; radray::RDGBufferHandle buffer;
        TextureRegion2D region; uint64_t offset; uint32_t pitch; bool toImage,depth;
        Texture* image{};
    };
    auto pass=graph.AddCopyPass("Raynium texture-buffer copy",Transfer{texture,buffer,region,offset,pitch,op.toImage,
        IsDepthStencilFormat(op.image.texture->GetDesc().Format)},+[](const Transfer& copy,radray::RDGCopyPassContext& ctx) noexcept {
            const auto slice=ctx.ResolveBuffer(copy.buffer);
            auto& cmd=ctx.GetCommandBuffer();
            const auto address=slice.Offset+copy.offset;
            if(copy.toImage) return cmd.CopyBufferToTextureRegion({slice.Buffer.Get(),copy.image,{address,copy.pitch,copy.region}});
            if(copy.depth) {
                cmd.CopyTextureToBuffer(slice.Buffer.Get(),address,copy.image,{copy.region.ArrayLayer,1,copy.region.MipLevel,1,TextureAspect::Depth});
                return cmd.IsValid();
            }
            return cmd.CopyTextureToBufferRegion({copy.image,slice.Buffer.Get(),{copy.region,address,copy.pitch}});
        },+[](Transfer& copy,radray::RDGPrepareContext& ctx) noexcept { copy.image=&ctx.Resolve(copy.texture); return true; });
    if(op.toImage) {
        pass.ReadBuffer(buffer).WriteTexture(texture);
        graph.MarkOutput(texture);
    } else {
        pass.ReadTexture(texture).WriteBuffer(buffer);
        if(intermediate) {
            for(uint32_t y=0;y<region.Height;++y) CopyBytes(graph,buffer,uint64_t(y)*pitch,real,op.offset+y*op.pitch,row);
        }
        graph.MarkOutput(real);
    }
}
void CommandTape::Replay(radray::RDGBuilder& graph,FrameTextureImports& imports,radray::AsyncReadbackQueue& queue,const OutputPassResources& output,const FrameTimestamps& timestamps,const FrameReadbacks& readbacks,Texture* backBuffer) {
    for(auto& view:_views) view.Import(graph,_backend);
    for(auto& command:_commands) {
        if(auto* clear=std::get_if<Clear>(&command)) {
            const auto& target=clear->target;
            auto handle=imports.Import(target.texture,target.full && !(GetTextureFormatAspects(target.texture->GetDesc().Format)&TextureAspect::Stencil));
            const auto& r=target.area;
            const bool depth=IsDepthStencilFormat(target.texture->GetDesc().Format);
            const radray::RDGAttachmentSlice slice{r.MipLevel,r.ArrayLayer,1};
            std::optional<radray::Rect> rect;
            if(!target.full) rect=radray::Rect{int32_t(r.X),int32_t(r.Y),r.Width,r.Height};
            if(depth) graph.AddClearDepthStencil(handle,{clear->value.Value[0],0},slice,true,false,rect);
            else graph.AddClearColor(handle,clear->value,slice,rect);
            graph.MarkOutput(handle);
        } else if(auto* read=std::get_if<Capture>(&command)) {
            auto handle=imports.Import(read->source.texture);
            if(!queue.EnqueueReserved(graph,read->id,handle,read->source.area)) RADRAY_ABORT("Prepared texture readback rejected");
        } else if(auto* upload=std::get_if<Upload>(&command)) {
            auto handle=imports.Import(upload->target.texture,upload->target.full);
            graph.UploadTexture(handle,upload->target.area,radray::RDGTextureUploadSource::OwnedBytes(std::move(upload->pixels),upload->pitch));
            graph.MarkOutput(handle);
        } else if(auto* write=std::get_if<WriteBuffer>(&command)) {
            if(write->target->host.empty()) {
                auto dst=write->target->Import(graph);
                graph.UploadBuffer(dst,write->offset,radray::RDGUploadSource::OwnedBytes(std::move(write->bytes)));
                graph.MarkOutput(dst);
            }
        } else if(auto* copy=std::get_if<CopyBuffer>(&command)) {
            auto src=copy->source->Import(graph);
            bool toHost=!copy->destination->host.empty();
            auto dst=toHost?readbacks.Import(graph):copy->destination->Import(graph);
            if(copy->source==copy->destination) {
                auto temp=graph.CreateBuffer({.Size=copy->size,.Memory=MemoryType::Device,.Usage=BufferUse::CopySource|BufferUse::CopyDestination});
                CopyBytes(graph,src,copy->srcOffset,temp,0,copy->size);
                CopyBytes(graph,temp,0,dst,copy->dstOffset,copy->size);
            } else CopyBytes(graph,src,copy->srcOffset,dst,toHost?copy->stagingOffset:copy->dstOffset,copy->size);
            graph.MarkOutput(dst);
        } else if(auto* image=std::get_if<TextureBuffer>(&command)) {
            ReplayTextureBuffer(graph,imports,readbacks,*image);
        } else if(auto* raster=std::get_if<radray::shared_ptr<ImmediatePass>>(&command)) {
            (*raster)->Replay(graph,imports,timestamps);
        } else if(auto* timestamp=std::get_if<Timestamp>(&command)) {
            timestamps.Write(graph,timestamp->physicalIndex);
        } else if(auto* blit=std::get_if<Blit>(&command)) {
            if(backBuffer) {
                const auto& r=blit->source.area;
                auto source=imports.Import(blit->source.texture);
                auto destination=graph.ImportTexture(backBuffer,TextureState::Undefined,TextureState::Present);
                output.AddPass(graph,source,destination,std::min(r.Width,backBuffer->GetDesc().Width),
                    std::min(r.Height,backBuffer->GetDesc().Height),r.MipLevel,true);
                graph.MarkOutput(destination);
            }
        } else {
            const auto& imageCopy=std::get<Copy>(command);
            auto src=imports.Import(imageCopy.source.texture);
            auto dst=imports.Import(imageCopy.destination.texture,imageCopy.destination.full);
            const auto& r=imageCopy.source.area;
            if(imageCopy.source.texture.Get()==imageCopy.destination.texture.Get()) {
                // RDG tracks copy states for the entire resource; take a snapshot before switching it to destination.
                auto temp=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=r.Width,.Height=r.Height,.DepthOrArraySize=1,
                    .MipLevels=1,.SampleCount=1,.Format=imageCopy.source.texture->GetDesc().Format,
                    .Usage=TextureUse::CopySource|TextureUse::CopyDestination});
                TextureRegion2D origin{0,0,0,0,r.Width,r.Height};
                CopyRegion(graph,src,r,temp,origin);
                CopyRegion(graph,temp,origin,dst,imageCopy.destination.area);
            } else CopyRegion(graph,src,r,dst,imageCopy.destination.area);
            graph.MarkOutput(dst);
        }
    }
}
}
