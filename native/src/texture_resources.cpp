#include <raynium/texture_resources.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <radray/runtime/frame_scheduler.h>
#include <utility>

namespace raynium {
using namespace radray::render;
namespace {
TextureFormat Format(rn_pixel_format format) noexcept {
    switch(format) {
    case RN_PIXEL_RGBA8:return TextureFormat::RGBA8_UNORM;
    case RN_PIXEL_R8:return TextureFormat::R8_UNORM;
    case RN_PIXEL_RGBA16F:return TextureFormat::RGBA16_FLOAT;
    case RN_PIXEL_RGBA32F:return TextureFormat::RGBA32_FLOAT;
    case RN_PIXEL_DEPTH32F:return TextureFormat::D32_FLOAT;
    default:return TextureFormat::UNKNOWN;
    }
}
}
radray::IntrusivePtr<TextureResource> TextureResource::Create(Device& device,const rn_uploaded_texture_desc& desc) {
    const auto format=Format(desc.format);
    auto dimension=desc.array_layers==1?TextureDimension::Dim2D:TextureDimension::Dim2DArray;
    if(!desc.flags && !IsDepthStencilFormat(format)) {
        auto texture=radray::GpuTexture::Create(device,{.Dim=dimension,.Width=desc.width,.Height=desc.height,
            .ArrayLayers=desc.array_layers,.MipLevels=desc.mip_levels,.Format=format}).Release();
        return texture?radray::IntrusivePtr<TextureResource>{new TextureResource(std::move(texture))}:nullptr;
    }
    if(desc.flags&RN_TEXTURE_CUBE) dimension=desc.array_layers==6?TextureDimension::Cube:TextureDimension::CubeArray;
    auto usage=TextureUse::CopyDestination|TextureUse::Resource;
    if(desc.flags&RN_TEXTURE_READBACK) usage|=TextureUse::CopySource;
    if(desc.flags&RN_TEXTURE_ATTACHMENT) {
        if(IsDepthStencilFormat(format)) usage|=TextureUse::DepthStencilRead|TextureUse::DepthStencilWrite;
        else usage|=TextureUse::RenderTarget;
    }
    const TextureDescriptor descriptor{.Dim=dimension,.Width=desc.width,.Height=desc.height,.DepthOrArraySize=desc.array_layers,
        .MipLevels=desc.mip_levels,.SampleCount=1,.Format=format,.Memory=MemoryType::Device,.Usage=usage};
    const auto [valid,error]=ValidateTextureDescriptor(descriptor,device);
    if(!valid) { RADRAY_ERR_LOG("Invalid scene texture: {}",error); return nullptr; }
    auto texture=device.CreateTexture(descriptor).Release();
    if(!texture) return nullptr;
    auto view=device.CreateTextureView({.Target=texture.get(),.Dim=dimension,.Format=format,
        .Range={0,desc.array_layers,0,desc.mip_levels,IsDepthStencilFormat(format)?TextureAspect::Depth:TextureAspect::Color},.Usage=TextureViewUsage::Resource}).Release();
    if(!view) return nullptr;
    return radray::IntrusivePtr<TextureResource>{new TextureResource(std::move(texture),std::move(view))};
}
TextureView* TextureResource::GetSamplingView(Device& device,const rn_texture_sampling& sampling) {
    auto* texture=GetTexture(); const auto& desc=texture->GetDesc();
    if(sampling.base_mip>=desc.MipLevels || sampling.mip_count>desc.MipLevels-sampling.base_mip) {
        RADRAY_ERR_LOG("Sampling view mip range exceeds texture"); return nullptr;
    }
    const uint32_t count=sampling.mip_count?sampling.mip_count:desc.MipLevels-sampling.base_mip;
    if(!sampling.base_mip && count==desc.MipLevels) return GetDefaultView();
    auto& view=_samplingViews[(uint64_t(sampling.base_mip)<<32)|count];
    if(!view) view=device.CreateTextureView({.Target=texture,.Dim=desc.Dim,.Format=desc.Format,
        .Range={0,desc.DepthOrArraySize,sampling.base_mip,count,IsDepthStencilFormat(desc.Format)?TextureAspect::Depth:TextureAspect::Color},
        .Usage=TextureViewUsage::Resource}).Release();
    return view.get();
}
Sampler* GetTextureSampler(Device& device,radray::unordered_map<uint64_t,radray::unique_ptr<Sampler>>& cache,
    uint32_t bits,const rn_texture_sampling& sampling) {
    if((bits&~63u) || !std::isfinite(sampling.max_lod) || sampling.max_lod<0 || !sampling.max_anisotropy ||
        sampling.max_anisotropy>device.GetCapabilities().Limits.MaxSamplerAnisotropy) {
        RADRAY_ERR_LOG("Invalid texture sampler LOD or anisotropy"); return nullptr;
    }
    // Minecraft's Vulkan sampler keeps a quarter-level range even when mipmaps are disabled.
    const float lod=std::max(0.25f,(bits&32)?0.f:sampling.max_lod);
    const uint64_t key=(uint64_t(std::bit_cast<uint32_t>(lod))<<32)|(uint64_t(sampling.max_anisotropy)<<8)|bits;
    auto& sampler=cache[key];
    if(!sampler) sampler=device.CreateSampler({.AddressS=(bits&1)?AddressMode::Repeat:AddressMode::ClampToEdge,
        .AddressT=(bits&2)?AddressMode::Repeat:AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
        .MinFilter=(bits&4)?FilterMode::Linear:FilterMode::Nearest,.MagFilter=(bits&8)?FilterMode::Linear:FilterMode::Nearest,
        .MipmapFilter=(bits&16)?FilterMode::Linear:FilterMode::Nearest,.LodMax=lod,.AnisotropyClamp=sampling.max_anisotropy}).Release();
    return sampler.get();
}
radray::RDGTextureHandle TextureResource::Import(radray::RDGBuilder& graph,radray::FrameSlot& slot,radray::vector<TextureResource*>& imports) {
    if(_frameId==slot.GetFrameId()) return _handle;
    _handle=_uploaded?graph.ImportPersistentOwned(_uploaded):graph.ImportTexture(_texture.get(),_state,TextureState::ShaderRead);
    if(!_uploaded && _state==TextureState::Undefined) {
        const auto& desc=_texture->GetDesc();
        if(desc.Dim==TextureDimension::Dim2D && desc.MipLevels==1 && bool(desc.Usage&TextureUse::RenderTarget)) {
            struct Clear {};
            graph.AddRasterPass("Raynium Initialize Color Target",Clear{},+[](const Clear&,radray::RDGRasterPassContext&) noexcept { return true; },
                +[](Clear&,radray::RDGPrepareContext&) noexcept { return true; })
                .WriteColor(0,_handle,LoadAction::Clear,StoreAction::Store,{{0,0,0,0}});
        }
    }
    slot.Emplace<radray::IntrusivePtr<TextureResource>>(this);
    imports.push_back(this); _frameId=slot.GetFrameId();
    return _handle;
}
void TextureResource::PublishSubmittedState() noexcept {
    if(!_uploaded) _state=TextureState::ShaderRead;
}
bool UploadedTextures::Create(radray::DeviceContext& runtime,const rn_uploaded_texture_desc& desc) {
    uint32_t levels=1;
    for(uint32_t side=std::max(desc.width,desc.height);side>1;side>>=1) ++levels;
    auto& device=runtime.GetDevice();
    if(desc.struct_size!=sizeof(desc) || desc.id<16 || _used.contains(desc.id) || desc.format>RN_PIXEL_DEPTH32F ||
       !desc.width || !desc.height || !desc.array_layers || (desc.flags&~7u) ||
       ((desc.flags&RN_TEXTURE_CUBE) && (desc.array_layers%6 || desc.width!=desc.height)) ||
       desc.width>device.GetCapabilities().Limits.MaxTexture2DDimension ||
       desc.height>device.GetCapabilities().Limits.MaxTexture2DDimension || !desc.mip_levels || desc.mip_levels>levels) {
        RADRAY_ERR_LOG("Invalid uploaded texture descriptor, format or reused ID: id={} size={}x{} mips={} format={}",desc.id,desc.width,desc.height,desc.mip_levels,uint32_t(desc.format)); return false;
    }
    UploadedTexture value{desc,TextureResource::Create(device,desc)};
    if(!value.texture) { RADRAY_ERR_LOG("Cannot create uploaded texture: id={}",desc.id); return false; }
    _textures.emplace(desc.id,std::move(value)); _used.insert(desc.id); return true;
}
const UploadedTexture* UploadedTextures::Find(uint32_t id) const {
    auto found=_textures.find(id); return found==_textures.end()?nullptr:&found->second;
}
bool UploadedTextures::Upload(const rn_texture_upload_desc& desc) {
    const auto* owner=Find(desc.id);
    if(!owner || desc.struct_size!=sizeof(desc) || !desc.pixels || desc.reserved || desc.array_layer>=owner->desc.array_layers || desc.mip_level>=owner->desc.mip_levels || !desc.width || !desc.height) {
        RADRAY_ERR_LOG("Invalid upload descriptor or missing texture: id={} mip={}",desc.id,desc.mip_level); return false;
    }
    const uint32_t width=std::max(1u,owner->desc.width>>desc.mip_level),height=std::max(1u,owner->desc.height>>desc.mip_level);
    if(owner->desc.format==RN_PIXEL_DEPTH32F) { RADRAY_ERR_LOG("CPU depth texture upload is unsupported: id={}",desc.id); return false; }
    const uint64_t rowBytes=uint64_t(desc.width)*GetTextureFormatBytesPerPixel(Format(owner->desc.format));
    const uint64_t pitch=desc.row_pitch?desc.row_pitch:rowBytes;
    if(uint64_t(desc.x)+desc.width>width || uint64_t(desc.y)+desc.height>height || pitch<rowBytes ||
       desc.byte_size<pitch*(desc.height-1)+rowBytes) { RADRAY_ERR_LOG("Texture upload region, row pitch or byte size is invalid: id={} mip={} region={},{},{}x{} row_pitch={} byte_size={}",desc.id,desc.mip_level,desc.x,desc.y,desc.width,desc.height,pitch,desc.byte_size); return false; }
    Pending pending{owner->texture,desc,{}};
    pending.bytes.resize(size_t(rowBytes*desc.height));
    for(uint32_t row=0;row<desc.height;++row)
        std::memcpy(pending.bytes.data()+rowBytes*row,static_cast<const radray::byte*>(desc.pixels)+pitch*row,size_t(rowBytes));
    pending.desc.pixels=nullptr; pending.desc.row_pitch=uint32_t(rowBytes); pending.desc.byte_size=pending.bytes.size();
    _pending.push_back(std::move(pending)); return true;
}
bool UploadedTextures::Destroy(uint32_t id) {
    if(!_textures.erase(id)) { RADRAY_ERR_LOG("Texture ID is missing or already destroyed: {}",id); return false; }
    return true;
}
void UploadedTextures::AddUploads(radray::RDGBuilder& graph,radray::FrameSlot& slot,radray::vector<TextureResource*>& imports,rn_frame_metrics* metrics) {
    std::pmr::unordered_map<TextureResource*,radray::RDGTextureHandle> cubeStaging{&slot.GetScratchMemoryResource()};
    for(auto& upload:_pending) {
        if(metrics) metrics->texture_upload_bytes+=upload.bytes.size();
        const auto target=upload.target->Import(graph,slot,imports);
        const auto desc=upload.target->GetTexture()->GetDesc();
        const bool cube=desc.Dim==TextureDimension::Cube || desc.Dim==TextureDimension::CubeArray;
        auto staging=target;
        if(cube) {
            auto [found,inserted]=cubeStaging.try_emplace(upload.target.Get());
            if(inserted) {
                auto temporary=desc; temporary.Dim=TextureDimension::Dim2DArray;
                temporary.Usage=TextureUse::CopyDestination|TextureUse::CopySource;
                found->second=graph.CreateTexture(temporary);
            }
            staging=found->second;
        }
        const TextureRegion2D region{upload.desc.mip_level,upload.desc.array_layer,upload.desc.x,upload.desc.y,upload.desc.width,upload.desc.height};
        graph.UploadTexture(staging,region,
            radray::RDGTextureUploadSource::OwnedBytes(std::move(upload.bytes),upload.desc.row_pitch));
        if(cube) {
            struct Copy { radray::RDGTextureHandle source,target; TextureRegion2D region; TextureCopyDescriptor copy{}; };
            graph.AddCopyPass("Raynium CubeUpload",Copy{staging,target,region},+[](const Copy& data,radray::RDGCopyPassContext& context) noexcept {
                context.GetCommandBuffer().CopyTextureToTexture(data.copy);
                return true;
            },+[](Copy& data,radray::RDGPrepareContext& context) noexcept {
                const auto& r=data.region;
                data.copy={.Destination=&context.Resolve(data.target),.DestinationMipLevel=r.MipLevel,
                    .DestinationArrayLayer=r.ArrayLayer,.DestinationX=r.X,.DestinationY=r.Y,.Source=&context.Resolve(data.source),
                    .SourceMipLevel=r.MipLevel,.SourceArrayLayer=r.ArrayLayer,.SourceX=r.X,.SourceY=r.Y,.Width=r.Width,.Height=r.Height};
                return true;
            }).ReadTexture(staging).WriteTexture(target);
        }
        graph.MarkOutput(target);
    }
    _pending.clear();
}
}
