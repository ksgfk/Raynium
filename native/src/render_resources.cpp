#include <raynium/render_resources.h>
#include <radray/profiler.h>
#include <algorithm>
#include <cstring>
#include <set>

namespace raynium {
using namespace radray::render;
namespace {
template<class T> bool Take(ExternalResult<T> result,T& output,radray::string& error) {
    if(auto* e=std::get_if<ExternalError>(&result)) { error=e->Message; return false; }
    output=std::get<T>(std::move(result)); return true;
}
}
RenderBuffer CreateBuffer(Device& d,uint64_t bytes,MemoryType memory,BufferUses uses,ResourceHints hints) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Buffer.Create");
    if(memory==MemoryType::Upload) uses|=BufferUse::MapWrite;
    return d.CreateBuffer({.Size=bytes,.Memory=memory,.Usage=uses,.Hints=hints}).Release();
}
MappedUploadBuffer::~MappedUploadBuffer() noexcept {
    if(_buffer && _data) _buffer->Unmap();
}
bool MappedUploadBuffer::Create(Device& device,uint64_t capacity,BufferUses usage,radray::unique_ptr<MappedUploadBuffer>& output,radray::string& error) {
    if(output) { error="Mapped upload output occupied"; return false; }
    if(!capacity) { error="Mapped upload capacity must be positive"; return false; }
    auto buffer=CreateBuffer(device,capacity,MemoryType::Upload,usage,ResourceHint::PersistentMap);
    if(!buffer) { error="Cannot create mapped upload buffer"; return false; }
    auto* data=static_cast<std::byte*>(buffer->Map(0,capacity));
    if(!data) { error="Cannot map upload buffer"; return false; }
    output=radray::unique_ptr<MappedUploadBuffer>(new MappedUploadBuffer(std::move(buffer),data,capacity,usage)); return true;
}
bool MappedUploadBuffer::Reserve(Device& device,uint64_t bytes,radray::string& error) {
    if(bytes<=_capacity) return true;
    uint64_t capacity=_capacity ? _capacity : 1;
    while(capacity<bytes) capacity<<=1;
    radray::unique_ptr<MappedUploadBuffer> replacement;
    if(!Create(device,capacity,_usage,replacement,error)) return false;
    // Only callers that confirmed the owner's previous GPU use completed reach this point.
    if(_buffer && _data) _buffer->Unmap();
    _data=nullptr;
    _buffer=std::move(replacement->_buffer);
    _data=replacement->_data;
    _capacity=capacity;
    replacement->_data=nullptr;
    return true;
}
void MappedUploadBuffer::Flush(uint64_t offset,uint64_t bytes) noexcept {
    if(!bytes) return;
    _buffer->FlushMappedRange({offset,bytes});
}
bool WriteBuffer(Buffer& buffer,const void* data,size_t bytes) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Buffer.Write");
    if(!bytes) return true;
    void* mapped;
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Buffer.Map");
        mapped=buffer.Map(0,bytes);
    }
    if(!mapped) return false;
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Buffer.Copy");
        std::memcpy(mapped,data,bytes);
    }
    {
        RADRAY_PROFILE_SCOPE_N("Raynium.Buffer.FlushUnmap");
        buffer.FlushMappedRange({0,bytes}); buffer.Unmap();
    }
    return true;
}
void TextureBarrier(CommandBuffer& c,Texture* texture,TextureStates before,TextureStates after,ExternalOwnershipTransfer transfer) {
    const ResourceBarrierDescriptor b=BarrierTextureDescriptor{.Target=texture,.Before=before,.After=after,
        .BeforeStages=ShaderStage::Vertex|ShaderStage::Pixel,.AfterStages=ShaderStage::Vertex|ShaderStage::Pixel,.ExternalTransfer=transfer};
    c.ResourceBarrier({&b,1});
}
void BufferBarrier(CommandBuffer& c,Buffer* buffer,BufferStates before,BufferStates after) {
    const ResourceBarrierDescriptor b=BarrierBufferDescriptor{.Target=buffer,.Before=before,.After=after}; c.ResourceBarrier({&b,1});
}
rn_shared_image_info DescribeImage(const ExternalTextureExport& texture,const ExternalFenceExport& ready,const ExternalFenceExport& consumed) {
    const auto& e=texture.Info; rn_shared_image_info info{};
    info.struct_size=sizeof(info); info.handle_type=e.SourceBackend==RenderBackend::Vulkan ? RN_SHARED_OPAQUE_WIN32 : RN_SHARED_D3D12;
    info.width=e.Descriptor.Width; info.height=e.Descriptor.Height;
    info.memory_handle=texture.Handle.Borrow().Value; info.ready_handle=ready.Handle.Borrow().Value; info.consumed_handle=consumed.Handle.Borrow().Value;
    info.allocation_size=e.Allocation.Size; info.allocation_offset=e.Allocation.Offset; info.dedicated=e.Allocation.Dedicated; info.node_mask=e.Device.NodeMask;
    std::copy(e.Device.Luid.begin(),e.Device.Luid.end(),info.luid); info.memory_type_index=UINT32_MAX;
    if(e.VulkanMemory) {
        info.memory_type_index=e.VulkanMemory->MemoryTypeIndex;
        std::copy(e.VulkanMemory->Device.DeviceUuid.begin(),e.VulkanMemory->Device.DeviceUuid.end(),info.device_uuid);
        std::copy(e.VulkanMemory->Device.DriverUuid.begin(),e.VulkanMemory->Device.DriverUuid.end(),info.driver_uuid);
    }
    info.native_format=e.NativeFormat; info.native_usage=e.NativeUsage; info.native_flags=e.NativeFlags; info.native_tiling=e.NativeTiling;
    return info;
}
bool RenderTarget::Create(RayniumRuntime& runtime,const rn_render_target_desc& desc,radray::unique_ptr<RenderTarget>& out,radray::string& error) {
    if(out || desc.struct_size!=sizeof(desc) || desc.reserved || !desc.width || !desc.height || desc.width>16384 || desc.height>16384) {
        error="Render target dimensions must be 1..16384"; return false;
    }
    auto& d=runtime.GetDevice(); auto ext=d.GetExternalResourceExtension();
    if(!ext) { error="External resources unavailable"; return false; }
    const bool vk=d.GetBackend()==RenderBackend::Vulkan;
    const ExternalTextureDescriptor external{vk ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource};
    const auto ft=vk ? ExternalFenceHandleType::OpaqueWin32 : ExternalFenceHandleType::D3D12Fence;
    TextureDescriptor td{.Dim=TextureDimension::Dim2D,.Width=desc.width,.Height=desc.height,.DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,
        .Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::RenderTarget|TextureUse::CopySource};
    auto support=ext->QueryTextureSupport(td,external);
    if(!support.CanCreateExportable) { error=support.Reason.Message; return false; }
    Resources r;
    if(!Take(ext->CreateExportableTexture(td,external),r.color,error) || !Take(ext->ExportTexture(r.color.get()),r.exported,error) ||
       !Take(ext->CreateExportableFence({ft,0}),r.ready,error) || !Take(ext->CreateExportableFence({ft,0}),r.consumed,error) ||
       !Take(ext->ExportFence(r.ready.get()),r.readyExport,error) || !Take(ext->ExportFence(r.consumed.get()),r.consumedExport,error)) return false;
    r.colorView=d.CreateTextureView({r.color.get(),TextureDimension::Dim2D,TextureFormat::RGBA8_UNORM,{0,1,0,1},
        TextureViewUsage::RenderTarget}).Release();
    if(!r.colorView) { error="Cannot create output attachment view"; return false; }
    out=radray::unique_ptr<RenderTarget>(new RenderTarget(std::move(r),desc.width,desc.height)); return true;
}
bool RenderTarget::Finish(radray::string& error) {
    if(_submitted && (!_r.ready->WaitFor(_submitted,5000) || !_r.consumed->WaitFor(_submitted,5000))) {
        error="Output completion timed out; resources remain live"; return false;
    }
    return true;
}

bool SharedTextures::Create(RayniumRuntime& runtime,const rn_shared_textures_desc& desc,radray::unique_ptr<SharedTextures>& out,radray::string& error) {
    if(out || desc.struct_size!=sizeof(desc) || !desc.texture_count || desc.texture_count>16 || !desc.textures) { error="Invalid shared texture set"; return false; }
    for(uint32_t i=0;i<desc.texture_count;++i) {
        const auto& t=desc.textures[i]; uint32_t levels=1;
        for(uint32_t side=std::max(t.width,t.height);side>1;side>>=1) ++levels;
        if(!t.width || !t.height || t.width>16384 || t.height>16384 || !t.mip_levels || t.mip_levels>levels || t.split_mips>1) {
            error="Invalid texture dimensions or mip chain"; return false;
        }
    }
    auto& d=runtime.GetDevice(); auto ext=d.GetExternalResourceExtension();
    if(!ext) { error="External resources unavailable"; return false; }
    const bool vk=d.GetBackend()==RenderBackend::Vulkan;
    const ExternalTextureDescriptor external{vk ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource};
    const auto ft=vk ? ExternalFenceHandleType::OpaqueWin32 : ExternalFenceHandleType::D3D12Fence;
    Sync sync;
    if(!Take(ext->CreateExportableFence({ft,0}),sync.released,error) || !Take(ext->CreateExportableFence({ft,0}),sync.available,error) ||
       !Take(ext->ExportFence(sync.released.get()),sync.releasedExport,error) || !Take(ext->ExportFence(sync.available.get()),sync.availableExport,error)) return false;
    std::vector<Image> images; std::vector<Logical> logical;
    for(uint32_t i=0;i<desc.texture_count;++i) {
        const auto& t=desc.textures[i];
        TextureDescriptor td{.Dim=TextureDimension::Dim2D,.Width=t.width,.Height=t.height,.DepthOrArraySize=1,.MipLevels=t.mip_levels,.SampleCount=1,
            .Format=TextureFormat::RGBA8_UNORM,.Memory=MemoryType::Device,.Usage=TextureUse::CopyDestination|TextureUse::CopySource|TextureUse::Resource};
        Logical item{t,{},{},false}; Texture* sampled=nullptr;
        if(t.split_mips) { item.assembled=d.CreateTexture(td).Unwrap(); sampled=item.assembled.get(); }
        for(uint32_t mip=0;mip<(t.split_mips ? t.mip_levels : 1);++mip) {
            auto shared=td;
            if(t.split_mips) { shared.Width=std::max(1u,t.width>>mip); shared.Height=std::max(1u,t.height>>mip); shared.MipLevels=1; }
            auto support=ext->QueryTextureSupport(shared,external);
            if(!support.CanCreateExportable) { error=support.Reason.Message; return false; }
            Image image{{},{},i,mip};
            if(!Take(ext->CreateExportableTexture(shared,external),image.texture,error) || !Take(ext->ExportTexture(image.texture.get()),image.exported,error)) return false;
            if(!t.split_mips) sampled=image.texture.get();
            images.push_back(std::move(image));
        }
        if(sampled) item.view=d.CreateTextureView({sampled,TextureDimension::Dim2D,td.Format,{0,1,0,t.mip_levels},TextureViewUsage::Resource}).Unwrap();
        if(!item.view) { error="Cannot create logical texture view"; return false; }
        item.sampled=sampled; logical.push_back(std::move(item));
    }
    sync.allocator=d.CreateCommandAllocator(&runtime.GetQueue()).Unwrap();
    if(sync.allocator) sync.command=d.CreateCommandBuffer(sync.allocator.get()).Unwrap();
    if(!sync.command) { error="Cannot create texture initialization commands"; return false; }
    auto owner=radray::unique_ptr<SharedTextures>(new SharedTextures(std::move(images),std::move(logical),std::move(sync)));
    auto& c=*owner->_sync.command; c.Begin();
    for(auto& image:owner->_images) {
        TextureBarrier(c,image.texture.get(),TextureState::Undefined,TextureState::CopyDestination);
        TextureBarrier(c,image.texture.get(),TextureState::CopyDestination,TextureState::Common,ExternalOwnershipTransfer::Release);
    }
    c.End();
    CommandBuffer* commands[]{&c}; Fence* signals[]{owner->_sync.released.get()}; uint64_t one=1;
    runtime.GetQueue().Submit({.CmdBuffers=commands,.SignalFences=signals,.SignalValues={&one,1}});
    out=std::move(owner); return true;
}
void SharedTextures::DescribeExports(rn_shared_image_export* output) const noexcept {
    for(const auto& image:_images) *output++={DescribeImage(image.exported,_sync.releasedExport,_sync.availableExport),
        image.logical,image.mip,_logical[image.logical].desc.split_mips ? 1u : _logical[image.logical].desc.mip_levels,0};
}
bool SharedTextures::Finish(radray::string& error) {
    if(!_sync.released->WaitFor(_released,5000)) { error="Shared input completion timed out; resources remain live"; return false; }
    return true;
}
bool SharedTextures::Validate(uint64_t sequence,uint32_t changed,radray::string& error) const {
    if(sequence!=_released || sequence==UINT64_MAX || (changed>>Count())) { error="Invalid input sequence or texture change mask"; return false; }
    for(uint32_t i=0;i<Count();++i) if(!_logical[i].initialized && !(changed&(1u<<i))) {
        error="First use requires initialized texture contents"; return false;
    }
    return true;
}
void SharedTextures::Acquire(CommandBuffer& command,uint32_t changed) {
    for(auto& image:_images) {
        const bool split=_logical[image.logical].desc.split_mips!=0;
        TextureBarrier(command,image.texture.get(),TextureState::Common,
            split ? TextureState::CopySource : TextureState::ShaderRead,ExternalOwnershipTransfer::Acquire);
    }
    for(uint32_t i=0;i<Count();++i) {
        auto& l=_logical[i]; if(!l.desc.split_mips) continue;
        if(!(changed&(1u<<i))) continue;
        TextureBarrier(command,l.assembled.get(),l.initialized ? TextureState::ShaderRead : TextureState::Undefined,TextureState::CopyDestination);
        for(auto& image:_images) if(image.logical==i) {
            command.CopyTextureToTexture({.Destination=l.assembled.get(),.DestinationMipLevel=image.mip,
                .Source=image.texture.get(),.Width=std::max(1u,l.desc.width>>image.mip),.Height=std::max(1u,l.desc.height>>image.mip)});
        }
        TextureBarrier(command,l.assembled.get(),TextureState::CopyDestination,TextureState::ShaderRead);
    }
}
void SharedTextures::Release(CommandBuffer& command) {
    for(auto& image:_images) TextureBarrier(command,image.texture.get(),
        _logical[image.logical].desc.split_mips ? TextureState::CopySource : TextureState::ShaderRead,
        TextureState::Common,ExternalOwnershipTransfer::Release);
}
void SharedTextures::Commit(uint32_t changed) noexcept {
    ++_released; for(uint32_t i=0;i<Count();++i) if(changed&(1u<<i)) _logical[i].initialized=true;
}

}
