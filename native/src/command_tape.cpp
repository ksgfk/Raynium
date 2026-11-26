#include <raynium/command_tape.h>
#include <raynium/raster_records.h>
#include <raynium/batch_envelope.h>
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

enum class Opcode : uint32_t { ClearColor=1, ClearDepth, CaptureColor, UploadTexture, CopyTexture, BlitToSurface, WriteTimestamp, WriteBufferBytes, CopyBufferBytes, BufferToTexture, TextureToBuffer, BeginPass, EndPass, Draw, RetainedTerrain, InstancedGeometry };
struct Header { Opcode opcode; uint32_t size; };
struct WireRegion { rn_texture* id; uint32_t mip,layer,x,y,width,height; };
struct WireClear { Header header; WireRegion target; float value[4]; };
struct WireCapture { Header header; WireRegion source; uint64_t id; };
struct WireUpload { Header header; WireRegion target; uint32_t pitch,reserved; uint64_t offset; };
struct WireWriteBuffer { Header header; rn_buffer* id; uint64_t offset,size,uploadOffset; };
struct WireBlit { Header header; rn_texture* texture; uint32_t mip,reserved; };
struct WireCopy { Header header; WireRegion source,destination; };
static_assert(sizeof(WireClear)==56 && sizeof(WireCapture)==48 && sizeof(WireUpload)==56 && sizeof(WireCopy)==72);

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

void FrameTapePlan::Measure(rn_frame_metrics& metrics) const {
    for(const auto& view:_views)
        for(const auto& geometry:view.uploads) metrics.terrain_upload_bytes+=geometry.source.GetMaxSize();
    for(const auto& geometry:_worldUploads) metrics.scene_upload_bytes+=geometry.source.GetMaxSize();
    metrics.scene_upload_bytes+=_instanceUploads.size();
    metrics.scene_upload_bytes+=uint64_t(_geometryIndirectArgs.size())*sizeof(DrawIndexedIndirectArguments);
    for(const auto& command:_commands) {
        if(const auto* raster=command.Get<RasterPassPlan>()) raster->Measure(metrics);
        else if(const auto* upload=command.Get<Upload>()) metrics.texture_upload_bytes+=upload->pixels.size();
        else if(const auto* write=command.Get<WriteBuffer>()) metrics.scene_upload_bytes+=write->borrowed.size();
    }
}
bool TapeCompiler::PrepareViews(const rn_batch_desc& batch,TerrainResidency& terrain,rn_renderer* owner,const MeshPassResources& mesh,
    bool indirect) {
    auto& slot=_slot;
    if(batch.struct_size!=sizeof(batch) || batch.view_reserved || (batch.view_count && !batch.views) || batch.view_count>65536) return false;
    _viewStorage.reserve(batch.view_count);
    for(size_t i=0;i<batch.view_count;++i) _viewStorage.emplace_back(slot.GetMemoryResource());
    _views=_viewStorage;
    _backend=slot.GetDevice().GetBackend();
    const auto& caps=slot.GetDevice().GetCapabilities();
    for(size_t i=0;i<_views.size();++i) {
        auto& view=_views[i];
        auto& cache=terrain.View(i);
        view.draws=&cache.draws;
        view.mesh=&mesh; view.vertexPool=&terrain.VertexPool(); view.translucentVertexPool=&terrain.TranslucentVertexPool(); view.quadIndexPool=&terrain.QuadIndexPool(); view.translucentIndexPool=&terrain.TranslucentIndexPool();
        view.capabilities=&caps; view.useIndirect=indirect;
        view.indirectLimit=caps.Limits.MaxDrawIndirectCount;
        if(!terrain.PrepareFrame(owner,batch.views[i],mesh,{slot,view.uploads,cache,indirect},view.frame)) return false;
    }
    return true;
}
bool TapeCompiler::Compile(const rn_batch_desc& batch,rn_renderer* owner,const StaticGeometryRegistry& staticGeometry,const TerrainResidency& terrain,FrameReadbacks& frame,const std::function<bool(uint64_t,uint32_t,uint32_t)>& reserveCapture,FrameTimestamps& timestamps) {
    auto& device=_slot.GetDevice();
    const auto& caps=device.GetCapabilities();
    auto& memory=_slot.GetMemoryResource();
    std::pmr::vector<TapeOp> ops(&_slot.GetScratchMemoryResource());
    std::pmr::vector<RasterOp> rasterOps(&_slot.GetScratchMemoryResource());
    auto append=[&]<class T>(T payload) -> decltype(auto) {
        if constexpr(TapeOp::template kInline<T>) {
            ops.emplace_back(payload);
        } else {
            auto* entry=FrameArray<T>(memory,1).data();
            *entry=payload;
            ops.emplace_back(entry);
            return *entry;
        }
    };
    radray::InlineVector<Clear,4> pendingClears;
    const auto flushClears=[&](RasterPassPlan* pass=nullptr) {
        for(const auto& clear:pendingClears) {
            const auto& target=clear.target;
            if(!pass || !pass->TryFoldClear(target.texture,target.area.MipLevel,target.area.ArrayLayer,clear.value))
                append(clear);
        }
        pendingClears.clear();
    };
    const uint32_t maxColors=caps.Limits.MaxColorAttachments;
    const bool canGeometryIndirect=caps.Features.IndirectDraw && caps.Features.IndirectFirstInstance && caps.Limits.MaxDrawIndirectCount>=2;
    const uint32_t indirectLimit=caps.Limits.MaxDrawIndirectCount;
    if(batch.struct_size!=sizeof(batch) || (batch.flags&~RN_BATCH_MEASURE) || batch.reserved || batch.capture_reserved || batch.query_reserved ||
       (batch.dirty_count && !batch.dirty_ranges) ||
       (batch.command_bytes && !batch.commands) || (batch.upload_bytes && !batch.uploads) ||
       (batch.capture_count && !batch.captures) || batch.capture_count>4096 || (batch.query_count && !batch.queries) ||
       batch.command_bytes>PTRDIFF_MAX || batch.upload_bytes>PTRDIFF_MAX || (batch.instance_upload_bytes && !batch.instance_uploads) || batch.instance_upload_bytes>PTRDIFF_MAX) {
        RADRAY_ERR_LOG("Invalid command batch descriptor");
        return false;
    }
    const auto resolve=[&](const WireRegion& wire,Region& result) {
        auto* entry=ResolveHandle(owner,wire.id);
        if(!entry || wire.mip>=entry->desc.mip_levels || wire.layer>=entry->desc.array_layers) return false;
        const uint32_t width=std::max(1u,entry->desc.width>>wire.mip),height=std::max(1u,entry->desc.height>>wire.mip);
        if(!wire.width || !wire.height || uint64_t(wire.x)+wire.width>width || uint64_t(wire.y)+wire.height>height) return false;
        result={entry,{wire.mip,wire.layer,wire.x,wire.y,wire.width,wire.height},wire.width==width && wire.height==height};
        return true;
    };
    std::span commands{static_cast<const radray::byte*>(batch.commands),size_t(batch.command_bytes)};
    const std::span uploads{static_cast<const radray::byte*>(batch.uploads),size_t(batch.upload_bytes)};
    _instanceUploads={static_cast<const radray::byte*>(batch.instance_uploads),size_t(batch.instance_upload_bytes)};
    bool hasBlit=false;
    uint32_t captures=0,queries=0;
    RasterPassPlan* raster=nullptr;
    while(!commands.empty()) {
        Header header{};
        if(commands.size()<sizeof(header)) return false;
        std::memcpy(&header,commands.data(),sizeof(header));
        if(header.size<sizeof(header) || header.size>commands.size() || header.size%8) return false;
        const auto bytes=commands.first(header.size);
        commands=commands.subspan(header.size);
        if(raster && header.opcode!=Opcode::RetainedTerrain && header.opcode!=Opcode::InstancedGeometry &&
           header.opcode!=Opcode::Draw &&
           header.opcode!=Opcode::WriteTimestamp && header.opcode!=Opcode::EndPass) return false;
        // Buffer-only transfers cannot observe or modify a pending texture clear.
        const bool bufferOnly=header.opcode==Opcode::WriteBufferBytes || header.opcode==Opcode::CopyBufferBytes;
        if(!bufferOnly && header.opcode!=Opcode::BeginPass && header.opcode!=Opcode::ClearColor && header.opcode!=Opcode::ClearDepth)
            flushClears();
        Region region{};
        switch(header.opcode) {
        case Opcode::ClearColor:
        case Opcode::ClearDepth: {
            WireClear wire{};
            if(!Read(bytes,wire) || !resolve(wire.target,region)) return false;
            const auto& desc=region.texture->texture->GetDesc();
            bool depth=header.opcode==Opcode::ClearDepth;
            if(depth!=IsDepthStencilFormat(desc.Format) || !(desc.Usage&(depth?TextureUse::DepthStencilWrite:TextureUse::RenderTarget))) return false;
            if(depth && !(wire.value[0]>=0 && wire.value[0]<=1)) return false;
            ColorClearValue value{};
            std::copy_n(wire.value,4,value.Value.begin());
            if(!region.full || (GetTextureFormatAspects(desc.Format)&TextureAspect::Stencil)) {
                flushClears();
                append(Clear{region,value});
                break;
            }
            const auto previous=std::find_if(pendingClears.begin(),pendingClears.end(),[&](const Clear& clear) {
                return clear.target.texture==region.texture && clear.target.area.MipLevel==region.area.MipLevel &&
                    clear.target.area.ArrayLayer==region.area.ArrayLayer;
            });
            if(previous!=pendingClears.end()) previous->value=value;
            else {
                if(pendingClears.size()==4) {
                    append(pendingClears.front());
                    std::move(pendingClears.begin()+1,pendingClears.end(),pendingClears.begin());
                    pendingClears.pop_back();
                }
                pendingClears.push_back({region,value});
            }
            break;
        }
        case Opcode::CaptureColor: {
            WireCapture wire{};
            if(!Read(bytes,wire) || !wire.id || captures>=batch.capture_count || batch.captures[captures++]!=wire.id ||
               !resolve(wire.source,region)) return false;
            const auto& desc=region.texture->texture->GetDesc();
            if(desc.Format!=TextureFormat::RGBA8_UNORM || !(desc.Usage&TextureUse::CopySource)) return false;
            if(!reserveCapture(wire.id,region.area.Width,region.area.Height)) return false;
            append(Capture{region,wire.id});
            break;
        }
        case Opcode::UploadTexture: {
            WireUpload wire{};
            if(!Read(bytes,wire) || !resolve(wire.target,region)) return false;
            const auto format=region.texture->texture->GetDesc().Format;
            if(IsDepthStencilFormat(format)) return false;
            const uint64_t row=uint64_t(wire.target.width)*GetTextureFormatBytesPerPixel(format);
            const uint64_t size=uint64_t(wire.pitch)*(wire.target.height-1)+row;
            if(wire.pitch<row || wire.offset>uploads.size() || size>uploads.size()-wire.offset) return false;
            auto pixels=uploads.subspan(wire.offset,size);
            append(Upload{region,pixels,wire.pitch});
            break;
        }
        case Opcode::CopyTexture: {
            WireCopy wire{};
            Region destination{};
            if(!Read(bytes,wire) || !resolve(wire.source,region) || !resolve(wire.destination,destination)) return false;
            const auto& src=region.texture->texture->GetDesc();
            if(!(src.Usage&TextureUse::CopySource) || src.Format!=destination.texture->texture->GetDesc().Format ||
                wire.source.width!=wire.destination.width || wire.source.height!=wire.destination.height ||
                (GetTextureFormatAspects(src.Format)&TextureAspect::Stencil) || (IsDepthStencilFormat(src.Format) && !(region.full && destination.full))) return false;
            const auto& a=wire.source;
            const auto& b=wire.destination;
            if(a.id==b.id && a.mip==b.mip && a.layer==b.layer && a.x<b.x+b.width && b.x<a.x+a.width &&
               a.y<b.y+b.height && b.y<a.y+a.height) return false;
            append(Copy{region,destination});
            break;
        }
        case Opcode::BlitToSurface: {
            WireBlit wire{};
            if(!Read(bytes,wire) || !batch.surface || hasBlit) return false;
            auto* entry=ResolveHandle(owner,wire.texture);
            if(!entry || wire.mip>=entry->desc.mip_levels || entry->desc.array_layers!=1 ||
               !(entry->desc.flags&RN_TEXTURE_READBACK) || IsDepthStencilFormat(entry->texture->GetDesc().Format) ||
               IsUintFormat(entry->texture->GetDesc().Format) || IsSintFormat(entry->texture->GetDesc().Format)) return false;
            hasBlit=true;
            append(Blit{{entry,{wire.mip,0,0,0,std::max(1u,entry->desc.width>>wire.mip),
                std::max(1u,entry->desc.height>>wire.mip)},true}});
            break;
        }
        case Opcode::WriteTimestamp: {
            struct WireTimestamp { Header header; rn_query_pool* pool; uint32_t slot,reserved; } wire{};
            if(!Read(bytes,wire) || wire.reserved || queries>=batch.query_count || _timestamps.size()==UINT32_MAX) return false;
            const auto& declared=batch.queries[queries++];
            if(declared.reserved || declared.pool!=wire.pool || declared.slot!=wire.slot) return false;
            auto* pool=ResolveHandle(owner,wire.pool);
            if(!pool || wire.slot>=pool->storage->slots.size()) return false;
            if(raster) raster->WriteTimestamp(rasterOps,uint32_t(_timestamps.size()));
            else append(Timestamp{uint32_t(_timestamps.size())});
            _timestamps.push_back({pool->storage,wire.slot});
            break;
        }
        case Opcode::WriteBufferBytes: {
            WireWriteBuffer wire{};
            if(!Read(bytes,wire)) return false;
            auto target=ResolveHandle(owner,wire.id);
            if(!target || !(target->usage&RN_BUFFER_COPY_DST) || !target->Contains(wire.offset,wire.size) ||
               wire.uploadOffset>uploads.size() || wire.size>uploads.size()-wire.uploadOffset) return false;
            auto pixels=uploads.subspan(wire.uploadOffset,wire.size);
            if(target->IsReadback()) frame.AddInline(target,wire.offset,{pixels.begin(),pixels.end()});
            append(WriteBuffer{target,wire.offset,pixels});
            break;
        }
        case Opcode::CopyBufferBytes: {
            struct Wire { Header header; rn_buffer *source,*destination; uint64_t srcOffset,dstOffset,size; } wire{};
            if(!Read(bytes,wire)) return false;
            auto src=ResolveHandle(owner,wire.source),dst=ResolveHandle(owner,wire.destination);
            if(!src || !dst || !(src->usage&RN_BUFFER_COPY_SRC) || !(dst->usage&RN_BUFFER_COPY_DST) ||
               !src->Contains(wire.srcOffset,wire.size) || !dst->Contains(wire.dstOffset,wire.size) ||
               (src==dst && wire.srcOffset<wire.dstOffset+wire.size && wire.dstOffset<wire.srcOffset+wire.size)) return false;
            auto& copy=append(CopyBuffer{src,dst,wire.srcOffset,wire.dstOffset,wire.size,0});
            if(dst->IsReadback() && !frame.Reserve(dst,wire.dstOffset,wire.size,1,wire.size,
                std::max<uint64_t>(1,caps.Limits.BufferCopyOffsetAlignment),copy.stagingOffset)) return false;
            break;
        }
        case Opcode::BufferToTexture:
        case Opcode::TextureToBuffer: {
            struct Wire { Header header; WireRegion image; rn_buffer* buffer; uint64_t offset,pitch; } wire{};
            if(!Read(bytes,wire) || !resolve(wire.image,region)) return false;
            const bool toImage=header.opcode==Opcode::BufferToTexture;
            auto buffer=ResolveHandle(owner,wire.buffer);
            const auto& image=region.texture->texture->GetDesc();
            const uint64_t row=uint64_t(region.area.Width)*GetTextureFormatBytesPerPixel(image.Format);
            if(!buffer || !(buffer->usage&(toImage?RN_BUFFER_COPY_SRC:RN_BUFFER_COPY_DST)) ||
               (!toImage && !(image.Usage&TextureUse::CopySource)) || wire.pitch>UINT32_MAX || wire.pitch<row ||
               (!toImage && wire.pitch!=row) || !buffer->Contains(wire.offset,wire.pitch*(region.area.Height-1)+row) ||
               (IsDepthStencilFormat(image.Format) && (toImage || !region.full || (GetTextureFormatAspects(image.Format)&TextureAspect::Stencil)))) return false;
            auto& transfer=append(TextureBuffer{region,std::move(buffer),wire.offset,wire.pitch,0,0,toImage});
            auto& desc=transfer.image.texture->texture->GetDesc();
            const auto& area=transfer.image.area;
            uint64_t minimum=IsDepthStencilFormat(desc.Format)?4u:1u;
            const uint64_t texel=GetTextureFormatBytesPerPixel(desc.Format);
            const uint64_t alignment=std::lcm(texel,std::max<uint64_t>(minimum,caps.Limits.TextureDataPitchAlignment));
            const uint64_t placement=std::lcm(texel,std::max<uint64_t>(minimum,caps.Limits.TextureDataPlacementAlignment));
            const uint64_t pitch=(row+alignment-1)/alignment*alignment;
            if(pitch>UINT32_MAX) return false;
            transfer.alignedPitch=uint32_t(pitch);
            transfer.direct=transfer.offset%placement==0 && transfer.pitch%alignment==0 &&
                transfer.buffer->Contains(transfer.offset,transfer.pitch*area.Height);
            if(transfer.buffer->IsReadback() && !frame.Reserve(transfer.buffer,transfer.offset,row,area.Height,pitch,placement,transfer.stagingOffset))
                return false;
            break;
        }
        case Opcode::RetainedTerrain:
            if(!raster || !raster->AddTerrain(memory,rasterOps,bytes,_views,owner)) return false;
            break;
        case Opcode::InstancedGeometry:
            if(!raster || !raster->AddInstancedGeometry(memory,rasterOps,bytes,_instanceUploads,_args,canGeometryIndirect,indirectLimit,owner,staticGeometry,terrain)) return false;
            break;
        case Opcode::Draw:
            if(!raster || !raster->AddDraw(memory,rasterOps,bytes,owner)) return false;
            break;
        case Opcode::BeginPass:
            raster=RasterPassPlan::Decode(memory,bytes,owner,maxColors);
            if(!raster) return false;
            flushClears(raster);
            ops.emplace_back(raster);
            break;
        case Opcode::EndPass:
            if(!raster || bytes.size()!=sizeof(Header)) return false;
            raster->Finish(memory,rasterOps);
            rasterOps.clear();
            raster=nullptr;
            break;
        default: return false;
        }
    }
    flushClears();
    _commands=FreezeFrameArray(memory,ops);
    _geometryIndirectArgs=_args;
    return !raster && hasBlit==(batch.surface!=nullptr) && captures==batch.capture_count && queries==batch.query_count &&
        frame.Prepare(device) && timestamps.Prepare(device,_slot.GetQueue(),_timestamps);
}

void FrameTapePlan::ReplayTextureBuffer(radray::RDGBuilder& graph,FrameTextureImports& imports,const FrameReadbacks& readbacks,const TextureBuffer& op) {
    auto texture=imports.Import(op.image.texture,op.toImage && op.image.full);
    const auto& region=op.image.area;
    const uint64_t row=uint64_t(region.Width)*GetTextureFormatBytesPerPixel(op.image.texture->texture->GetDesc().Format);
    const bool host=op.buffer->IsReadback();
    if(op.toImage && !op.direct && op.buffer->IsHostVisible()) {
        const auto length=(region.Height-1)*op.pitch+row;
        graph.UploadTexture(texture,region,radray::RDGTextureUploadSource::BorrowedBytes(
            {op.buffer->mapped+op.offset,size_t(length)},uint32_t(op.pitch)));
        graph.MarkOutput(texture);
        return;
    }
    auto real=host?readbacks.Import(graph):op.buffer->Import(graph);
    const bool intermediate=!host && !op.direct;
    auto buffer=intermediate?graph.CreateBuffer({.Size=uint64_t(op.alignedPitch)*region.Height,
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
        IsDepthStencilFormat(op.image.texture->texture->GetDesc().Format)},+[](const Transfer& copy,radray::RDGCopyPassContext& ctx) noexcept {
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
void FrameTapePlan::Replay(radray::RDGBuilder& graph,FrameTextureImports& imports,radray::AsyncReadbackQueue& queue,const OutputPassResources& output,const FrameTimestamps& timestamps,const FrameReadbacks& readbacks,Texture* backBuffer) {
    for(auto& upload:_worldUploads) {
        auto h=graph.ImportPersistentOwned(upload.target);
        graph.UploadBuffer(h,upload.offset,std::move(upload.source));
        graph.MarkOutput(h);
    }

    radray::RDGBufferHandle instanceBuffer;
    if(!_instanceUploads.empty()) {
        instanceBuffer=graph.CreateDynamicBuffer({.MaxSize=_instanceUploads.size(),.Usage=BufferUse::Vertex,
            .Residency=StreamResidency(_instanceUploads.size())},
            radray::RDGUploadSource::BorrowedBytes(_instanceUploads));
    }
    radray::RDGBufferHandle geometryIndirectBuffer;
    if(!_geometryIndirectArgs.empty()) {
        const auto argBytes=std::as_bytes(std::span{_geometryIndirectArgs});
        geometryIndirectBuffer=graph.CreateDynamicBuffer({.MaxSize=argBytes.size(),.Usage=BufferUse::Indirect},
            radray::RDGUploadSource::BorrowedBytes(argBytes));
    }
    for(auto& view:_views) view.Import(graph,_backend);
    for(auto& command:_commands) {
        if(auto* clear=command.Get<Clear>()) {
            const auto& target=clear->target;
            auto handle=imports.Import(target.texture,target.full && !(GetTextureFormatAspects(target.texture->texture->GetDesc().Format)&TextureAspect::Stencil));
            const auto& r=target.area;
            const bool depth=IsDepthStencilFormat(target.texture->texture->GetDesc().Format);
            const radray::RDGAttachmentSlice slice{r.MipLevel,r.ArrayLayer,1};
            std::optional<radray::Rect> rect;
            if(!target.full) rect=radray::Rect{int32_t(r.X),int32_t(r.Y),r.Width,r.Height};
            if(depth) graph.AddClearDepthStencil(handle,{clear->value.Value[0],0},slice,true,false,rect);
            else graph.AddClearColor(handle,clear->value,slice,rect);
            graph.MarkOutput(handle);
        } else if(auto* read=command.Get<Capture>()) {
            auto handle=imports.Import(read->source.texture);
            if(!queue.EnqueueReserved(graph,read->id,handle,read->source.area))
                RADRAY_ABORT("Reserved capture readback failed to enqueue");
        } else if(auto* upload=command.Get<Upload>()) {
            auto handle=imports.Import(upload->target.texture,upload->target.full);
            graph.UploadTexture(handle,upload->target.area,radray::RDGTextureUploadSource::BorrowedBytes(upload->pixels,upload->pitch));
            graph.MarkOutput(handle);
        } else if(auto* write=command.Get<WriteBuffer>()) {
            if(!write->target->IsReadback()) {
                auto dst=write->target->Import(graph);
                graph.UploadBuffer(dst,write->offset,radray::RDGUploadSource::BorrowedBytes(write->borrowed));
                graph.MarkOutput(dst);
            }
        } else if(auto* copy=command.Get<CopyBuffer>()) {
            auto src=copy->source->Import(graph);
            bool toHost=copy->destination->IsReadback();
            auto dst=toHost?readbacks.Import(graph):copy->destination->Import(graph);
            if(copy->source==copy->destination) {
                auto temp=graph.CreateBuffer({.Size=copy->size,.Usage=BufferUse::CopySource|BufferUse::CopyDestination});
                CopyBytes(graph,src,copy->srcOffset,temp,0,copy->size);
                CopyBytes(graph,temp,0,dst,copy->dstOffset,copy->size);
            } else CopyBytes(graph,src,copy->srcOffset,dst,toHost?copy->stagingOffset:copy->dstOffset,copy->size);
            graph.MarkOutput(dst);
        } else if(auto* image=command.Get<TextureBuffer>()) {
            ReplayTextureBuffer(graph,imports,readbacks,*image);
        } else if(auto* raster=command.Get<RasterPassPlan>()) {
            raster->Replay(graph,imports,timestamps,instanceBuffer,geometryIndirectBuffer);
        } else if(auto timestamp=command.Get<Timestamp>()) {
            timestamps.Write(graph,timestamp->physicalIndex);
        } else if(auto* blit=command.Get<Blit>()) {
            if(backBuffer) {
                const auto& r=blit->source.area;
                auto source=imports.Import(blit->source.texture);
                auto destination=graph.ImportTexture(backBuffer,TextureState::Undefined,TextureState::Present);
                output.AddPass(graph,source,destination,std::min(r.Width,backBuffer->GetDesc().Width),
                    std::min(r.Height,backBuffer->GetDesc().Height),r.MipLevel,true);
                graph.MarkOutput(destination);
            }
        } else {
            const auto& imageCopy=*command.Get<Copy>();
            auto src=imports.Import(imageCopy.source.texture);
            auto dst=imports.Import(imageCopy.destination.texture,imageCopy.destination.full);
            const auto& r=imageCopy.source.area;
            if(imageCopy.source.texture==imageCopy.destination.texture) {
                // RDG tracks copy states for the entire resource; take a snapshot before switching it to destination.
                auto temp=graph.CreateTexture({.Dim=TextureDimension::Dim2D,.Width=r.Width,.Height=r.Height,.DepthOrArraySize=1,
                    .MipLevels=1,.SampleCount=1,.Format=imageCopy.source.texture->texture->GetDesc().Format,
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
