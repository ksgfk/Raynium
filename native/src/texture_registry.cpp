#include <raynium/texture_registry.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>

namespace raynium {
using namespace radray::gpu;
TextureFormat ToTextureFormat(rn_pixel_format format) noexcept {
    switch(format) {
    case RN_PIXEL_RGBA8:return TextureFormat::RGBA8_UNORM;
    case RN_PIXEL_R8:return TextureFormat::R8_UNORM;
    case RN_PIXEL_RGBA16F:return TextureFormat::RGBA16_FLOAT;
    case RN_PIXEL_RGBA32F:return TextureFormat::RGBA32_FLOAT;
    case RN_PIXEL_DEPTH32F:return TextureFormat::D32_FLOAT;
    case RN_PIXEL_R8_SNORM:return TextureFormat::R8_SNORM;
    case RN_PIXEL_RG8_UNORM:return TextureFormat::RG8_UNORM;
    case RN_PIXEL_RG8_SNORM:return TextureFormat::RG8_SNORM;
    case RN_PIXEL_RGBA8_SNORM:return TextureFormat::RGBA8_SNORM;
    case RN_PIXEL_R16_UNORM:return TextureFormat::R16_UNORM;
    case RN_PIXEL_R16_SNORM:return TextureFormat::R16_SNORM;
    case RN_PIXEL_RG16_UNORM:return TextureFormat::RG16_UNORM;
    case RN_PIXEL_RG16_SNORM:return TextureFormat::RG16_SNORM;
    case RN_PIXEL_RGBA16_UNORM:return TextureFormat::RGBA16_UNORM;
    case RN_PIXEL_RGBA16_SNORM:return TextureFormat::RGBA16_SNORM;
    case RN_PIXEL_R8_UINT:return TextureFormat::R8_UINT;
    case RN_PIXEL_R8_SINT:return TextureFormat::R8_SINT;
    case RN_PIXEL_RG8_UINT:return TextureFormat::RG8_UINT;
    case RN_PIXEL_RG8_SINT:return TextureFormat::RG8_SINT;
    case RN_PIXEL_RGBA8_UINT:return TextureFormat::RGBA8_UINT;
    case RN_PIXEL_RGBA8_SINT:return TextureFormat::RGBA8_SINT;
    case RN_PIXEL_R16_UINT:return TextureFormat::R16_UINT;
    case RN_PIXEL_R16_SINT:return TextureFormat::R16_SINT;
    case RN_PIXEL_RG16_UINT:return TextureFormat::RG16_UINT;
    case RN_PIXEL_RG16_SINT:return TextureFormat::RG16_SINT;
    case RN_PIXEL_RGBA16_UINT:return TextureFormat::RGBA16_UINT;
    case RN_PIXEL_RGBA16_SINT:return TextureFormat::RGBA16_SINT;
    case RN_PIXEL_R32_UINT:return TextureFormat::R32_UINT;
    case RN_PIXEL_R32_SINT:return TextureFormat::R32_SINT;
    case RN_PIXEL_RG32_UINT:return TextureFormat::RG32_UINT;
    case RN_PIXEL_RG32_SINT:return TextureFormat::RG32_SINT;
    case RN_PIXEL_RGBA32_UINT:return TextureFormat::RGBA32_UINT;
    case RN_PIXEL_RGBA32_SINT:return TextureFormat::RGBA32_SINT;
    case RN_PIXEL_R16_FLOAT:return TextureFormat::R16_FLOAT;
    case RN_PIXEL_RG16_FLOAT:return TextureFormat::RG16_FLOAT;
    case RN_PIXEL_R32_FLOAT:return TextureFormat::R32_FLOAT;
    case RN_PIXEL_RG32_FLOAT:return TextureFormat::RG32_FLOAT;
    case RN_PIXEL_RGB10A2_UNORM:return TextureFormat::RGB10A2_UNORM;
    case RN_PIXEL_RGB10A2_UINT:return TextureFormat::RGB10A2_UINT;
    case RN_PIXEL_RG11B10_FLOAT:return TextureFormat::RG11B10_FLOAT;
    case RN_PIXEL_D32_FLOAT_S8_UINT:return TextureFormat::D32_FLOAT_S8_UINT;
    case RN_PIXEL_D24_UNORM_S8_UINT:return TextureFormat::D24_UNORM_S8_UINT;
    case RN_PIXEL_D16_UNORM:return TextureFormat::D16_UNORM;
    default:return TextureFormat::UNKNOWN;
    }
}
radray::IntrusivePtr<radray::GpuTexture> CreateSceneTexture(Device& device,const rn_uploaded_texture_desc& desc) {
    const auto format=ToTextureFormat(desc.format);
    auto dimension=desc.array_layers==1?TextureDimension::Dim2D:TextureDimension::Dim2DArray;
    if(desc.flags&RN_TEXTURE_CUBE) dimension=desc.array_layers==6?TextureDimension::Cube:TextureDimension::CubeArray;
    auto usage=TextureUse::CopyDestination|TextureUse::Resource;
    if(desc.flags&RN_TEXTURE_READBACK) usage|=TextureUse::CopySource;
    if(desc.flags&RN_TEXTURE_ATTACHMENT) {
        if(IsDepthStencilFormat(format)) usage|=TextureUse::DepthStencilRead|TextureUse::DepthStencilWrite;
        else usage|=TextureUse::RenderTarget;
    }
    return radray::GpuTexture::Create(device,{.Dim=dimension,.Width=desc.width,.Height=desc.height,
        .ArrayLayers=desc.array_layers,.MipLevels=desc.mip_levels,.Format=format,.Usage=usage}).Release();
}
TextureView* GetSamplingView(Device& device,radray::GpuTexture& owner,const rn_texture_sampling& sampling) {
    auto* texture=owner.GetTexture(); const auto& desc=texture->GetDesc();
    if(sampling.base_mip>=desc.MipLevels || sampling.mip_count>desc.MipLevels-sampling.base_mip) {
        RADRAY_ERR_LOG("Sampling view mip range exceeds texture"); return nullptr;
    }
    const uint32_t count=sampling.mip_count?sampling.mip_count:desc.MipLevels-sampling.base_mip;
    if(!sampling.base_mip && count==desc.MipLevels) if(auto view=owner.GetDefaultView()) return view.Get();
    return owner.GetOrCreateView(device,{.Dim=desc.Dim,.Format=desc.Format,
        .Range={0,desc.DepthOrArraySize,sampling.base_mip,count,IsDepthStencilFormat(desc.Format)?TextureAspect::Depth:TextureAspect::Color},
        .Usage=TextureViewUsage::Resource}).Get();
}
Sampler* GetTextureSampler(Device& device,uint32_t bits,const rn_texture_sampling& sampling) {
    if((bits&~63u) || !std::isfinite(sampling.max_lod) || sampling.max_lod<0 || !sampling.max_anisotropy ||
        sampling.max_anisotropy>device.GetCapabilities().Limits.MaxSamplerAnisotropy) {
        RADRAY_ERR_LOG("Invalid texture sampler LOD or anisotropy"); return nullptr;
    }
    // Minecraft's Vulkan sampler keeps a quarter-level range even when mipmaps are disabled.
    const float lod=std::max(0.25f,(bits&32)?0.f:sampling.max_lod);
    return device.GetOrCreateSampler({.AddressS=(bits&1)?AddressMode::Repeat:AddressMode::ClampToEdge,
        .AddressT=(bits&2)?AddressMode::Repeat:AddressMode::ClampToEdge,.AddressR=AddressMode::ClampToEdge,
        .MinFilter=(bits&4)?FilterMode::Linear:FilterMode::Nearest,.MagFilter=(bits&8)?FilterMode::Linear:FilterMode::Nearest,
        .MipmapFilter=(bits&16)?FilterMode::Linear:FilterMode::Nearest,.LodMax=lod,.AnisotropyClamp=sampling.max_anisotropy}).Get();
}
FrameTextureImports::FrameTextureImports(radray::RDGBuilder& graph,std::pmr::memory_resource& scratch,const DeviceDetail* detail)
    :_graph(graph),_detail(detail),_cleared(&scratch) {}
radray::RDGTextureHandle FrameTextureImports::Import(const radray::IntrusivePtr<radray::GpuTexture>& texture,bool overwritesWholeSubresource) {
    const auto handle=_graph.ImportPersistentOwned(texture);
    const auto& desc=texture->GetDesc();
    if(_detail) {
        if(texture->GetSubmittedState()!=TextureState::Undefined || !_cleared.insert(texture.Get()).second ||
           (overwritesWholeSubresource && desc.ArrayLayers==1 && desc.MipLevels==1)) return handle;
        auto zero=+[](const int&,radray::RDGUploadWriteContext& ctx) noexcept {
            std::memset(ctx.Bytes.data(),0,ctx.Bytes.size());
            return true;
        };
        const auto aspects=GetTextureFormatAspects(desc.Format);
        for(uint32_t mip=0;mip<desc.MipLevels;++mip) {
            const uint32_t width=std::max(1u,desc.Width>>mip),height=std::max(1u,desc.Height>>mip);
            const uint64_t bytes=uint64_t(width)*GetTextureFormatBytesPerPixel(desc.Format);
            for(uint32_t layer=0;layer<desc.ArrayLayers;++layer) {
                const radray::RDGAttachmentSlice slice{mip,layer,1};
                if(desc.Usage&TextureUse::RenderTarget) _graph.AddClearColor(handle,{{0,0,0,0}},slice);
                else if(desc.Usage&TextureUse::DepthStencilWrite)
                    _graph.AddClearDepthStencil(handle,{0,0},slice,true,bool(aspects&TextureAspect::Stencil));
                else if(!IsDepthStencilFormat(desc.Format))
                    _graph.UploadTexture(handle,{mip,layer,0,0,width,height},radray::RDGTextureUploadSource::Generated(bytes*height,0,zero));
                else {
                    const uint64_t align=std::max(4u,_detail->TextureDataPitchAlignment);
                    const uint64_t size=((bytes+align-1)/align)*align*height;
                    auto src=_graph.CreateBuffer({.MaxSize=size,.Usage=BufferUse::CopySource,
                        .Alignment=std::max<uint64_t>(4,_detail->TextureDataPlacementAlignment)},radray::RDGUploadSource::Generated(size,0,zero));
                    struct Fill { radray::RDGTextureHandle dst; radray::RDGBufferHandle src; SubresourceRange range; Texture* image{}; };
                    SubresourceRange range{layer,1,mip,1,aspects};
                    for(auto aspect:{TextureAspect::Depth,TextureAspect::Stencil}) if(aspects&aspect) {
                        range.Aspects=aspect;
                        _graph.AddCopyPass("Initialize depth plane",Fill{handle,src,range},
                            +[](const Fill& fill,radray::RDGCopyPassContext& ctx) noexcept {
                                auto slice=ctx.ResolveBuffer(fill.src);
                                ctx.GetCommandBuffer().CopyBufferToTexture(fill.image,fill.range,slice.Buffer.Get(),slice.Offset);
                                return ctx.GetCommandBuffer().IsValid();
                            },+[](Fill& fill,radray::RDGPrepareContext& ctx) noexcept { fill.image=&ctx.Resolve(fill.dst); return true; })
                            .ReadBuffer(src).WriteTexture(handle);
                    }
                }
            }
        }
        return handle;
    }
    if(texture->GetSubmittedState()==TextureState::Undefined && desc.Dim==TextureDimension::Dim2D &&
       desc.MipLevels==1 && bool(desc.Usage&TextureUse::RenderTarget) && _cleared.insert(texture.Get()).second)
        _graph.AddClearColor(handle,{{0,0,0,0}});
    return handle;
}
bool TextureRegistry::Create(radray::DeviceContext& runtime,const rn_uploaded_texture_desc& desc) {
    uint32_t levels=1;
    for(uint32_t side=std::max(desc.width,desc.height);side>1;side>>=1) ++levels;
    auto& device=runtime.GetDevice();
    if(desc.struct_size!=sizeof(desc) || desc.id<16 || _used.contains(desc.id) || ToTextureFormat(desc.format)==TextureFormat::UNKNOWN ||
       !desc.width || !desc.height || !desc.array_layers || desc.array_layers>device.GetCapabilities().Limits.MaxTextureArrayLayers || (desc.flags&~7u) ||
       ((desc.flags&RN_TEXTURE_CUBE) && (desc.array_layers%6 || desc.width!=desc.height)) ||
       desc.width>device.GetCapabilities().Limits.MaxTexture2DDimension ||
       desc.height>device.GetCapabilities().Limits.MaxTexture2DDimension || !desc.mip_levels || desc.mip_levels>levels) {
        RADRAY_ERR_LOG("Invalid uploaded texture descriptor, format or reused ID: id={} size={}x{} mips={} format={}",desc.id,desc.width,desc.height,desc.mip_levels,uint32_t(desc.format)); return false;
    }
    UploadedTexture value{desc,CreateSceneTexture(device,desc)};
    if(!value.texture) { RADRAY_ERR_LOG("Cannot create uploaded texture: id={}",desc.id); return false; }
    _textures.emplace(desc.id,std::move(value)); _used.insert(desc.id); return true;
}
const UploadedTexture* TextureRegistry::Find(uint32_t id) const {
    auto found=_textures.find(id); return found==_textures.end()?nullptr:&found->second;
}
bool TextureRegistry::PrepareUpload(const rn_uploaded_texture_desc& texture,const rn_texture_upload_desc& desc,Upload& pending) {
    if(desc.struct_size!=sizeof(desc) || !desc.pixels || desc.reserved || desc.array_layer>=texture.array_layers || desc.mip_level>=texture.mip_levels || !desc.width || !desc.height) {
        RADRAY_ERR_LOG("Invalid upload descriptor or missing texture: id={} mip={}",desc.id,desc.mip_level); return false;
    }
    const uint32_t width=std::max(1u,texture.width>>desc.mip_level),height=std::max(1u,texture.height>>desc.mip_level);
    if(IsDepthStencilFormat(ToTextureFormat(texture.format))) { RADRAY_ERR_LOG("CPU depth texture upload is unsupported: id={}",desc.id); return false; }
    const uint64_t rowBytes=uint64_t(desc.width)*GetTextureFormatBytesPerPixel(ToTextureFormat(texture.format));
    const uint64_t pitch=desc.row_pitch?desc.row_pitch:rowBytes;
    if(uint64_t(desc.x)+desc.width>width || uint64_t(desc.y)+desc.height>height || pitch<rowBytes ||
       desc.byte_size<pitch*(desc.height-1)+rowBytes) { RADRAY_ERR_LOG("Texture upload region, row pitch or byte size is invalid: id={} mip={} region={},{},{}x{} row_pitch={} byte_size={}",desc.id,desc.mip_level,desc.x,desc.y,desc.width,desc.height,pitch,desc.byte_size); return false; }
    pending.desc=desc;
    pending.bytes.resize(size_t(rowBytes*desc.height));
    for(uint32_t row=0;row<desc.height;++row)
        std::memcpy(pending.bytes.data()+rowBytes*row,static_cast<const radray::byte*>(desc.pixels)+pitch*row,size_t(rowBytes));
    pending.desc.pixels=nullptr; pending.desc.row_pitch=uint32_t(rowBytes); pending.desc.byte_size=pending.bytes.size();
    return true;
}
void TextureRegistry::Enqueue(Upload upload) {
    const auto* texture=Find(upload.desc.id);
    if(!texture) RADRAY_ABORT("FIFO upload lost texture {}",upload.desc.id);
    _pending.push_back({texture->texture,upload.desc,std::move(upload.bytes)});
}
bool TextureRegistry::Destroy(uint32_t id) {
    if(!_textures.erase(id)) { RADRAY_ERR_LOG("Texture ID is missing or already destroyed: {}",id); return false; }
    return true;
}
void TextureRegistry::AddUploads(radray::RDGBuilder& graph,FrameTextureImports& imports,rn_frame_metrics* metrics) {
    for(auto& upload:_pending) {
        if(metrics) metrics->texture_upload_bytes+=upload.bytes.size();
        const auto& desc=upload.target->GetDesc();
        const auto target=imports.Import(upload.target,upload.desc.width==desc.Width && upload.desc.height==desc.Height);
        const TextureRegion2D region{upload.desc.mip_level,upload.desc.array_layer,upload.desc.x,upload.desc.y,upload.desc.width,upload.desc.height};
        graph.UploadTexture(target,region,radray::RDGTextureUploadSource::OwnedBytes(std::move(upload.bytes),upload.desc.row_pitch));
        graph.MarkOutput(target);
    }
    _pending.clear();
}
}
