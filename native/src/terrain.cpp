#include <raynium/terrain.h>
#include <raynium/terrain_bytecode.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <radray/logger.h>
#include <set>

namespace raynium {
using namespace radray::render;
namespace {
constexpr uint32_t MaxVertices = 1u << 20;
constexpr uint32_t PageVertices = (4u * 1024 * 1024 / 112) * 4;
template<class T> bool Take(ExternalResult<T> value, T& result, radray::string& error) {
    if (auto* e = std::get_if<ExternalError>(&value)) { error = e->Message; return false; }
    result = std::get<T>(std::move(value)); return true;
}
void TextureBarrier(CommandBuffer& c, Texture* t, TextureStates before, TextureStates after,
                    ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    const ResourceBarrierDescriptor b = BarrierTextureDescriptor{.Target=t, .Before=before, .After=after,
        .BeforeStages=ShaderStage::Vertex | ShaderStage::Pixel, .AfterStages=ShaderStage::Vertex | ShaderStage::Pixel,
        .ExternalTransfer=transfer};
    c.ResourceBarrier({&b, 1});
}
void BufferBarrier(CommandBuffer& c, Buffer* b, BufferStates before, BufferStates after) {
    const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{.Target=b, .Before=before, .After=after};
    c.ResourceBarrier({&barrier, 1});
}
radray::unique_ptr<Buffer> BufferOf(Device& d, uint64_t size, MemoryType memory, BufferUses usage) {
    if (memory==MemoryType::Upload) usage|=BufferUse::MapWrite;
    return d.CreateBuffer({size, memory, usage, memory == MemoryType::Upload ? ResourceHint::PersistentMap : ResourceHint::None}).Unwrap();
}
bool Write(Buffer& buffer, const void* data, size_t bytes) {
    if (!bytes) return true;
    auto* mapped = buffer.Map(0, bytes);
    if (!mapped) return false;
    std::memcpy(mapped, data, bytes); buffer.FlushMappedRange({0,bytes}); buffer.Unmap(); return true;
}
rn_preview_info Describe(const ExternalTextureExport& texture, const ExternalFenceExport& ready, const ExternalFenceExport& available) {
    const auto& e = texture.Info;
    rn_preview_info info{};
    info.struct_size=sizeof(info); info.handle_type=e.SourceBackend==RenderBackend::Vulkan ? RN_SHARED_OPAQUE_WIN32 : RN_SHARED_D3D12;
    info.width=e.Descriptor.Width; info.height=e.Descriptor.Height;
    info.memory_handle=texture.Handle.Borrow().Value; info.ready_handle=ready.Handle.Borrow().Value;
    info.consumed_handle=available.Handle.Borrow().Value;
    info.allocation_size=e.Allocation.Size; info.allocation_offset=e.Allocation.Offset;
    info.dedicated=e.Allocation.Dedicated; info.node_mask=e.Device.NodeMask;
    std::copy(e.Device.Luid.begin(), e.Device.Luid.end(), info.luid);
    info.memory_type_index=std::numeric_limits<uint32_t>::max();
    if (e.VulkanMemory) {
        info.memory_type_index=e.VulkanMemory->MemoryTypeIndex;
        std::copy(e.VulkanMemory->Device.DeviceUuid.begin(), e.VulkanMemory->Device.DeviceUuid.end(), info.device_uuid);
        std::copy(e.VulkanMemory->Device.DriverUuid.begin(), e.VulkanMemory->Device.DriverUuid.end(), info.driver_uuid);
    }
    info.native_format=e.NativeFormat; info.native_usage=e.NativeUsage; info.native_flags=e.NativeFlags; info.native_tiling=e.NativeTiling;
    return info;
}
}

bool Terrain::Create(RayniumRuntime& runtime, const rn_terrain_desc& desc, radray::unique_ptr<Terrain>& output,
                     rn_preview_info* images, radray::string& error) noexcept {
    auto& d=runtime.GetDevice(); auto ext=d.GetExternalResourceExtension();
    if (!ext || output) { error="Terrain external resources unavailable or output occupied"; return false; }
    const bool vk=d.GetBackend()==RenderBackend::Vulkan;
    const ExternalTextureDescriptor external{vk ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource};
    const auto ft=vk ? ExternalFenceHandleType::OpaqueWin32 : ExternalFenceHandleType::D3D12Fence;
    Resources r;
    if (!Take(ext->CreateExportableFence({ft, 0}), r.ready, error) || !Take(ext->CreateExportableFence({ft, 0}), r.available, error) ||
        !Take(ext->ExportFence(r.ready.get()), r.readyExport, error) || !Take(ext->ExportFence(r.available.get()), r.availableExport, error)) return false;
    const uint32_t atlasImages=desc.split_mips ? desc.mip_levels : 1;
    for (uint32_t i=0; i<=atlasImages; ++i) {
        const bool light=i==atlasImages;
        TextureDescriptor td{.Dim=TextureDimension::Dim2D, .Width=light ? 16u : std::max(1u, desc.width >> (desc.split_mips ? i : 0)),
            .Height=light ? 16u : std::max(1u, desc.height >> (desc.split_mips ? i : 0)), .DepthOrArraySize=1,
            .MipLevels=light || desc.split_mips ? 1u : desc.mip_levels, .SampleCount=1, .Format=TextureFormat::RGBA8_UNORM,
            .Memory=MemoryType::Device, .Usage=TextureUse::CopyDestination | TextureUse::CopySource | TextureUse::Resource};
        const auto support=ext->QueryTextureSupport(td, external);
        if (!support.CanCreateExportable) { error=support.Reason.Message; return false; }
        SharedImage image;
        if (!Take(ext->CreateExportableTexture(td, external), image.texture, error) ||
            !Take(ext->ExportTexture(image.texture.get()), image.exported, error)) return false;
        r.images.push_back(std::move(image));
    }
    Texture* atlas=r.images[0].texture.get();
    if (desc.split_mips) {
        auto td=atlas->GetDesc(); td.Width=desc.width; td.Height=desc.height; td.MipLevels=desc.mip_levels;
        r.privateAtlas=d.CreateTexture(td).Unwrap(); atlas=r.privateAtlas.get();
    }
    if (!atlas) { error="Cannot create fallback atlas"; return false; }
    r.atlasView=d.CreateTextureView({atlas, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, desc.mip_levels}, TextureViewUsage::Resource}).Unwrap();
    r.lightView=d.CreateTextureView({r.images.back().texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0,1,0,1}, TextureViewUsage::Resource}).Unwrap();
    SamplerDescriptor sampler{.AddressS=AddressMode::ClampToEdge, .AddressT=AddressMode::ClampToEdge, .AddressR=AddressMode::ClampToEdge,
        .MinFilter=FilterMode::Linear, .MagFilter=FilterMode::Linear, .MipmapFilter=FilterMode::Linear, .LodMax=float(desc.mip_levels-1)};
    r.atlasSampler=d.CreateSampler(sampler).Unwrap();
    sampler.MinFilter=sampler.MagFilter=FilterMode::Linear; sampler.LodMax=0;
    r.lightSampler=d.CreateSampler(sampler).Unwrap();
    const auto stages=ShaderStage::Vertex | ShaderStage::Pixel;
    const BindingDescriptor bindings[]{
        {0,BindingKind::CBuffer,1,stages,D3D12BindingDescriptor{BindingRegisterClass::CBV,0,0},VulkanBindingDescriptor{0,0}},
        {1,BindingKind::Texture,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::SRV,0,0},VulkanBindingDescriptor{0,1}},
        {2,BindingKind::Texture,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::SRV,1,0},VulkanBindingDescriptor{0,2}},
        {3,BindingKind::Sampler,1,ShaderStage::Pixel,D3D12BindingDescriptor{BindingRegisterClass::Sampler,0,0},VulkanBindingDescriptor{0,3}},
        {4,BindingKind::Sampler,1,ShaderStage::Vertex,D3D12BindingDescriptor{BindingRegisterClass::Sampler,1,0},VulkanBindingDescriptor{0,4}}};
    r.group=d.CreateBindingGroupLayout({bindings}).Unwrap();
    if (!r.group || !r.atlasView || !r.lightView || !r.atlasSampler || !r.lightSampler) { error="Cannot create terrain bindings"; return false; }
    const PipelineLayoutGroupDescriptor group{0,r.group.get()};
    r.layout=d.CreatePipelineLayout({{&group,1},{}}).Unwrap();
    const auto blob=vk ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
    r.vs=d.CreateShader({vk ? TerrainVsSpirv() : TerrainVsDxil(),blob,ShaderStage::Vertex}).Unwrap();
    r.ps=d.CreateShader({vk ? TerrainPsSpirv() : TerrainPsDxil(),blob,ShaderStage::Pixel}).Unwrap();
    const VertexBufferLayout layouts[]{{0,28,VertexStepMode::Vertex},{1,16,VertexStepMode::Instance}};
    const VertexAttribute attributes[]{
        {0,0,"POSITION",0,VertexFormat::FLOAT32X3,0}, {0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        {0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2}, {0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4}, {1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    auto primitive=PrimitiveState::Default(); primitive.FaceClockwise=FrontFace::CW; primitive.UnclippedDepth=false;
    auto depth=DepthStencilState::Default(); depth.DepthCompare=CompareFunction::GreaterEqual;
    const auto color=ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    if (!r.layout || !r.vs || !r.ps) { error="Cannot create terrain shaders"; return false; }
    r.pipeline=d.CreateGraphicsPipelineState({.PipelineLayout=r.layout.get(), .VS=ShaderEntry{r.vs.get(),"VSMain"},
        .PS=ShaderEntry{r.ps.get(),"PSMain"}, .VertexInput={layouts,attributes}, .Primitive=primitive,
        .DepthStencil=depth, .MultiSample=MultiSampleState::Default(), .ColorTargets={&color,1}}).Unwrap();
    if (!r.pipeline) { error="Cannot create SOLID pipeline"; return false; }
    for (auto& frame:r.frames) {
        if(d.GetCapabilities().Features.TimestampQueries) {
            frame.timestamps=d.CreateQueryPool({QueryType::Timestamp,3,"Raynium terrain"}).Unwrap();
            frame.timestampReadback=BufferOf(d,24,MemoryType::ReadBack,BufferUse::CopyDestination | BufferUse::MapRead);
            if(!frame.timestamps || !frame.timestampReadback) { error="Cannot create terrain timing resources"; return false; }
        }
        frame.constants=BufferOf(d,256,MemoryType::Upload,BufferUse::CBuffer);
        frame.bindings=d.CreateShaderParameterSet({r.group.get()}).Unwrap();
        if (!frame.constants || !frame.bindings ||
            !frame.bindings->Set(0,0,ShaderBufferBinding{frame.constants.get(),{0,256}}) ||
            !frame.bindings->Set(1,0,r.atlasView.get()) || !frame.bindings->Set(2,0,r.lightView.get()) ||
            !frame.bindings->Set(3,0,r.atlasSampler.get()) || !frame.bindings->Set(4,0,r.lightSampler.get()) || !frame.bindings->FlushWrites()) {
            error="Cannot create terrain frame constants"; return false;
        }
    }
    const uint64_t indexBytes=uint64_t(MaxVertices)*6;
    r.indices=BufferOf(d,indexBytes,MemoryType::Device,BufferUse::CopyDestination | BufferUse::Index);
    r.initialUpload=BufferOf(d,indexBytes,MemoryType::Upload,BufferUse::CopySource);
    r.bootstrapAllocator=d.CreateCommandAllocator(&runtime.GetQueue()).Unwrap();
    if (!r.indices || !r.initialUpload || !r.bootstrapAllocator) { error="Cannot create shared quad indices"; return false; }
    r.bootstrap=d.CreateCommandBuffer(r.bootstrapAllocator.get()).Unwrap();
    if (!r.bootstrap) { error="Cannot create initialization command buffer"; return false; }
    auto* indices=static_cast<uint32_t*>(r.initialUpload->Map(0,indexBytes));
    if (!indices) { error="Cannot map quad index staging"; return false; }
    for (uint32_t q=0;q<MaxVertices/4;++q) {
        const uint32_t pattern[]{0,1,2,2,3,0};
        for (uint32_t j=0;j<6;++j) indices[q*6+j]=q*4+pattern[j];
    }
    r.initialUpload->FlushMappedRange({0,indexBytes}); r.initialUpload->Unmap();
    r.bootstrap->Begin();
    BufferBarrier(*r.bootstrap,r.indices.get(),BufferState::Undefined,BufferState::CopyDestination);
    r.bootstrap->CopyBufferToBuffer(r.indices.get(),0,r.initialUpload.get(),0,indexBytes);
    BufferBarrier(*r.bootstrap,r.indices.get(),BufferState::CopyDestination,BufferState::Index);
    for (auto& image:r.images) {
        TextureBarrier(*r.bootstrap,image.texture.get(),TextureState::Undefined,TextureState::CopyDestination);
        TextureBarrier(*r.bootstrap,image.texture.get(),TextureState::CopyDestination,TextureState::Common,ExternalOwnershipTransfer::Release);
    }
    r.bootstrap->End();
    // Allocate the owner before any submission so allocation failure cannot free in-flight resources.
    radray::unique_ptr<Terrain> terrain{new(std::nothrow) Terrain(runtime,std::move(r),desc)};
    if (!terrain) { error="Cannot allocate terrain session"; return false; }
    auto& resources=terrain->_r;
    CommandBuffer* commands[]{resources.bootstrap.get()}; Fence* signals[]{resources.ready.get()}; uint64_t one=1;
    runtime.GetQueue().Submit({.CmdBuffers=commands,.SignalFences=signals,.SignalValues={&one,1}});
    for (size_t i=0;i<resources.images.size();++i) images[i]=Describe(resources.images[i].exported,resources.readyExport,resources.availableExport);
    output=std::move(terrain); return true;
}

bool Terrain::Finish(radray::string& error) noexcept {
    if (!_r.ready->WaitFor(_submitted,5000)) { error="Terrain GPU completion timed out; resources remain live"; return false; }
    return true;
}
bool Terrain::Validate(const rn_terrain_frame& f, radray::string& error) const noexcept {
    if((_desc.split_mips && !_privateInitialized && !(f.flags&2u)) || f.input_sequence==UINT64_MAX) {
        error="First fallback frame requires initialized atlas contents"; return false;
    }
    if (f.struct_size!=sizeof(f) || (f.flags & ~15u) || f.anisotropy>16 || (!f.epoch || f.epoch<_epoch) || f.input_sequence!=_submitted ||
        (f.update_count && !f.updates) || (f.draw_count && !f.draws) || (f.removed_count && !f.removed) ||
        f.update_count>65536 || f.draw_count>65536 || f.removed_count>1048576) { error="Invalid terrain frame or input sequence"; return false; }
    for (uint32_t i=0;i<f.update_count;++i) {
        const auto& m=f.updates[i];
        uint64_t groupCount=0;
        for(auto count:m.group_vertices) { if(count%4) { error="Face groups require complete quads"; return false; } groupCount+=count; }
        if (groupCount!=m.vertex_count || !m.id || !m.vertices || !m.vertex_count || m.vertex_count%4 || m.vertex_count>MaxVertices || m.reserved) {
            error="Invalid SOLID mesh; expected 28-byte quad vertices"; return false;
        }
    }
    for(uint32_t i=0;i<f.draw_count;++i) if(f.draws[i].reserved || f.draws[i].group_mask>127) { error="Invalid terrain face mask"; return false; }
    return true;
}
Terrain::Mesh Terrain::Allocate(uint32_t count) {
    for (uint32_t i=0;i<_pages.size();++i) {
        auto& p=_pages[i];
        for (auto it=p.free.begin();it!=p.free.end();++it) if (it->second>=count) {
            auto [offset, length]=*it; p.free.erase(it);
            if (length>count) p.free.emplace(offset+count,length-count);
            return {i,offset,count};
        }
    }
    const uint32_t capacity=std::max(PageVertices,count);
    auto buffer=BufferOf(_runtime.GetDevice(),uint64_t(capacity)*28,MemoryType::Device,BufferUse::CopyDestination | BufferUse::Vertex);
    if (!buffer) return {UINT32_MAX,0,0};
    Page page{std::move(buffer),{},false};
    if (capacity>count) page.free.emplace(count,capacity-count);
    _pages.push_back(std::move(page)); return {uint32_t(_pages.size()-1),0,count};
}
void Terrain::Reclaim() {
    const auto completed=_r.ready->GetCompletedValue();
    for (auto it=_retired.begin();it!=_retired.end();) {
        if (it->fence>completed) { ++it; continue; }
        auto& free=_pages[it->mesh.page].free;
        auto entry=free.emplace(it->mesh.offset,it->mesh.count).first;
        if (entry!=free.begin()) {
            auto before=std::prev(entry);
            if (before->first+before->second==entry->first) { before->second+=entry->second; free.erase(entry); entry=before; }
        }
        auto after=std::next(entry);
        if (after!=free.end() && entry->first+entry->second==after->first) { entry->second+=after->second; free.erase(after); }
        it=_retired.erase(it);
    }
}
bool Terrain::Prepare(CommandBuffer& command, uint32_t slot, const rn_terrain_frame& f, radray::string& error) noexcept {
    auto& d=_runtime.GetDevice(); auto& frame=_r.frames[slot];
    if(frame.measured) {
        auto* ticks=static_cast<uint64_t*>(frame.timestampReadback->Map(0,24));
        if(!ticks) { error="Cannot read terrain timing results"; return false; }
        frame.timestampReadback->InvalidateMappedRange({0,24});
        const double ms=frame.timestamps->GetTimestampCalibration(&_runtime.GetQueue()).TickPeriodNs/1e6;
        _gpuUploadMs.push_back(double(ticks[1]-ticks[0])*ms); _gpuDrawMs.push_back(double(ticks[2]-ticks[1])*ms);
        frame.timestampReadback->Unmap(); frame.measured=false;
        if(_gpuDrawMs.size()==120) {
            std::sort(_gpuUploadMs.begin(),_gpuUploadMs.end()); std::sort(_gpuDrawMs.begin(),_gpuDrawMs.end());
            uint64_t residentBytes=0,retiredBytes=0;
            for(auto& page:_pages) residentBytes+=page.buffer->GetDesc().Size;
            for(auto& retired:_retired) retiredBytes+=uint64_t(retired.mesh.count)*28;
            RADRAY_INFO_LOG("Terrain GPU samples=120 prepare_ms_p50={} p95={} p99={} raster_ms_p50={} p95={} p99={} pages={} page_bytes={} meshes={} retired_bytes={} draws={}",
                _gpuUploadMs[60],_gpuUploadMs[114],_gpuUploadMs[118],_gpuDrawMs[60],_gpuDrawMs[114],_gpuDrawMs[118],
                _pages.size(),residentBytes,_meshes.size(),retiredBytes,frame.args.size());
            _gpuUploadMs.clear(); _gpuDrawMs.clear();
        }
    }
    frame.measuring=(f.flags&8u) && frame.timestamps;
    if(frame.measuring) { command.ResetQueryPool(frame.timestamps.get(),0,3); command.WriteTimestamp({frame.timestamps.get(),QueryPipelineStage::Top,0}); }

    if (f.epoch!=_epoch) {
        for (auto [id,mesh]:_meshes) _retired.push_back({mesh,_submitted});
        _meshes.clear(); _epoch=f.epoch;
    }
    for (uint32_t i=0;i<f.removed_count;++i) if (auto it=_meshes.find(f.removed[i]);it!=_meshes.end()) {
        _retired.push_back({it->second,_submitted}); _meshes.erase(it);
    }
    Reclaim();
    uint64_t bytes=0;
    for (uint32_t i=0;i<f.update_count;++i) if (!_meshes.contains(f.updates[i].id)) bytes+=uint64_t(f.updates[i].vertex_count)*28;
    if (bytes>frame.stagingSize) {
        frame.staging=BufferOf(d,bytes,MemoryType::Upload,BufferUse::CopySource); frame.stagingSize=bytes;
        if (!frame.staging) { error="Cannot allocate terrain staging"; return false; }
    }
    auto* staging=bytes ? static_cast<std::byte*>(frame.staging->Map(0,bytes)) : nullptr;
    if (bytes && !staging) { error="Cannot map terrain staging"; return false; }
    struct Upload { Mesh mesh; uint64_t offset; }; std::vector<Upload> uploads; std::set<uint32_t> touched;
    uint64_t offset=0;
    for (uint32_t i=0;i<f.update_count;++i) {
        const auto& u=f.updates[i]; if (_meshes.contains(u.id)) continue;
        auto mesh=Allocate(u.vertex_count);
        std::copy_n(u.group_vertices,7,mesh.groups);
        if (mesh.page==UINT32_MAX) { if(bytes) frame.staging->Unmap(); error="Cannot allocate terrain page"; return false; }
        std::memcpy(staging+offset,u.vertices,size_t(u.vertex_count)*28);
        uploads.push_back({mesh,offset}); offset+=uint64_t(u.vertex_count)*28; touched.insert(mesh.page); _meshes.emplace(u.id,mesh);
    }
    if(bytes) { frame.staging->FlushMappedRange({0,bytes}); frame.staging->Unmap(); }
    for (auto p:touched) BufferBarrier(command,_pages[p].buffer.get(),_pages[p].initialized ? BufferState::Vertex : BufferState::Undefined,BufferState::CopyDestination);
    for (const auto& u:uploads) command.CopyBufferToBuffer(_pages[u.mesh.page].buffer.get(),uint64_t(u.mesh.offset)*28,frame.staging.get(),u.offset,uint64_t(u.mesh.count)*28);
    for (auto p:touched) { BufferBarrier(command,_pages[p].buffer.get(),BufferState::CopyDestination,BufferState::Vertex); _pages[p].initialized=true; }
    const uint32_t anisotropy=std::clamp(f.anisotropy,1u,uint32_t(d.GetCapabilities().Limits.MaxSamplerAnisotropy));
    if(frame.anisotropy!=anisotropy) {
        Sampler* sampler=_r.atlasSampler.get();
        if(anisotropy>1) {
            auto& cached=_r.anisotropicSamplers[anisotropy];
            if(!cached) cached=d.CreateSampler({.AddressS=AddressMode::ClampToEdge,.AddressT=AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
                .MinFilter=FilterMode::Linear,.MagFilter=FilterMode::Linear,.MipmapFilter=FilterMode::Linear,
                .LodMax=float(_desc.mip_levels-1),.AnisotropyClamp=anisotropy}).Unwrap();
            if(!cached) { error="Cannot create terrain anisotropic sampler"; return false; }
            sampler=cached.get();
        }
        frame.bindings->ResetBindingsForReuse();
        if(!frame.bindings->Set(0,0,ShaderBufferBinding{frame.constants.get(),{0,256}}) ||
            !frame.bindings->Set(1,0,_r.atlasView.get()) || !frame.bindings->Set(2,0,_r.lightView.get()) ||
            !frame.bindings->Set(3,0,sampler) || !frame.bindings->Set(4,0,_r.lightSampler.get()) || !frame.bindings->FlushWrites()) {
            error="Cannot update terrain sampler"; return false;
        }
        frame.anisotropy=anisotropy;
    }
    const uint64_t drawSize=std::max<uint64_t>(1,uint64_t(f.draw_count)*7);
    if (drawSize>frame.drawSize) {
        frame.instances=BufferOf(d,drawSize*16,MemoryType::Upload,BufferUse::Vertex);
        frame.indirect=BufferOf(d,drawSize*20,MemoryType::Upload,BufferUse::Indirect); frame.drawSize=drawSize;
        if (!frame.instances || !frame.indirect) { error="Cannot allocate terrain draw stream"; return false; }
    }
    struct Instance { int32_t origin[3]; float visibility; };
    std::vector<Instance> instances; frame.args.clear(); frame.pages.clear();
    struct OrderedDraw { uint32_t index, depthBin, page; };
    std::vector<OrderedDraw> order; order.reserve(f.draw_count);
    for(uint32_t i=0;i<f.draw_count;++i) if(auto it=_meshes.find(f.draws[i].id);it!=_meshes.end()) {
        double distance=0;
        for(int axis=0;axis<3;++axis) { double delta=double(f.draws[i].origin[axis])-f.camera_block[axis]+8; distance+=delta*delta; }
        order.push_back({i,uint32_t(std::sqrt(distance)/64.0),it->second.page});
    }
    std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b) {
        return a.depthBin!=b.depthBin ? a.depthBin<b.depthBin : a.page<b.page;
    });
    for (const auto& item:order) {
        const auto& draw=f.draws[item.index]; const auto& mesh=_meshes.at(draw.id);
        uint32_t first=0;
        for(uint32_t group=0;group<7;++group) {
            const uint32_t count=mesh.groups[group]/4*6;
            if(count && (draw.group_mask & (1u<<group))) {
                if(!frame.args.empty() && frame.args.back().FirstInstance==instances.size() &&
                    frame.args.back().FirstIndex+frame.args.back().IndexCount==first) frame.args.back().IndexCount+=count;
                else { frame.args.push_back({count,1,first,int32_t(mesh.offset),uint32_t(instances.size())}); frame.pages.push_back(mesh.page); }
            }
            first+=count;
        }
        Instance instance{}; std::copy_n(draw.origin,3,instance.origin); instance.visibility=draw.visibility; instances.push_back(instance);
    }
    if (!instances.empty() && (!Write(*frame.instances,instances.data(),instances.size()*sizeof(Instance)) ||
        !Write(*frame.indirect,frame.args.data(),frame.args.size()*20))) { error="Cannot write terrain draw stream"; return false; }
    struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4]; } constants{};
    std::copy_n(f.projection,16,constants.projection); std::copy_n(f.view,16,constants.view); std::copy_n(f.camera_block,3,constants.block); constants.block[3]=(f.flags&4u)!=0;
    std::copy_n(f.camera_fraction,3,constants.fraction); constants.fraction[3]=d.GetBackend()==RenderBackend::Vulkan ? 1.0f : -1.0f;
    std::copy_n(f.fog_color,4,constants.fog); std::copy_n(f.fog_distances,4,constants.distances);
    if (!Write(*frame.constants,&constants,sizeof(constants))) { error="Cannot write terrain constants"; return false; }
    for (size_t i=0;i<_r.images.size();++i) TextureBarrier(command,_r.images[i].texture.get(),TextureState::Common,
        _desc.split_mips && i+1<_r.images.size() ? TextureState::CopySource : TextureState::ShaderRead,ExternalOwnershipTransfer::Acquire);
    if (_desc.split_mips && (f.flags & 2u)) {
        TextureBarrier(command,_r.privateAtlas.get(),_privateInitialized ? TextureState::ShaderRead : TextureState::Undefined,TextureState::CopyDestination);
        for (uint32_t mip=0;mip<_desc.mip_levels;++mip) command.CopyTextureToTexture({.Destination=_r.privateAtlas.get(),.DestinationMipLevel=mip,
            .Source=_r.images[mip].texture.get(),.Width=std::max(1u,_desc.width>>mip),.Height=std::max(1u,_desc.height>>mip)});
        TextureBarrier(command,_r.privateAtlas.get(),TextureState::CopyDestination,TextureState::ShaderRead); _privateInitialized=true;
    }
    if(frame.measuring) command.WriteTimestamp({frame.timestamps.get(),QueryPipelineStage::Bottom,1});
    return true;
}
void Terrain::Render(GraphicsCommandEncoder& pass, uint32_t slot, bool direct) noexcept {
    auto& frame=_r.frames[slot]; pass.BindGraphicsPipelineState(_r.pipeline.get()); pass.BindShaderParameterSet(0,frame.bindings.get());
    pass.BindIndexBuffer({_r.indices.get(),0,4});
    for (uint32_t i=0;i<frame.args.size();) {
        const uint32_t page=frame.pages[i], start=i;
        do { ++i; } while(i<frame.args.size() && frame.pages[i]==page);
        const VertexBufferBinding bindings[]{{0,{_pages[page].buffer.get(),0,_pages[page].buffer->GetDesc().Size}},
            {1,{frame.instances.get(),0,frame.drawSize*16}}};
        pass.BindVertexBuffers(bindings);
        if (!direct) pass.DrawIndexedIndirect(frame.indirect.get(),uint64_t(start)*20,i-start);
        else for(uint32_t j=start;j<i;++j) { const auto& a=frame.args[j]; pass.DrawIndexed(a.IndexCount,1,a.FirstIndex,a.VertexOffset,a.FirstInstance); }
    }
}
void Terrain::Release(CommandBuffer& command,uint32_t slot) noexcept {
    auto& frame=_r.frames[slot];
    if(frame.measuring) {
        command.WriteTimestamp({frame.timestamps.get(),QueryPipelineStage::Bottom,2});
        BufferBarrier(command,frame.timestampReadback.get(),frame.timingReadbackInitialized ? BufferState::HostRead : BufferState::Undefined,BufferState::CopyDestination);
        frame.timingReadbackInitialized=true;
        command.ResolveQueryData({frame.timestamps.get(),0,3,frame.timestampReadback.get(),0});
        BufferBarrier(command,frame.timestampReadback.get(),BufferState::CopyDestination,BufferState::HostRead);
        frame.measured=true;
    }
    for (size_t i=0;i<_r.images.size();++i) TextureBarrier(command,_r.images[i].texture.get(),
        _desc.split_mips && i+1<_r.images.size() ? TextureState::CopySource : TextureState::ShaderRead,
        TextureState::Common,ExternalOwnershipTransfer::Release);
}
}
