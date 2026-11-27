#include <raynium/resource_uploads.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>

#include <radray/logger.h>
#include <radray/types.h>

namespace raynium {
radray::gpu::TextureFormat ToTextureFormat(rn_pixel_format format) noexcept {
    switch (format) {
        case RN_PIXEL_RGBA8: return radray::gpu::TextureFormat::RGBA8_UNORM;
        case RN_PIXEL_R8: return radray::gpu::TextureFormat::R8_UNORM;
        case RN_PIXEL_RGBA16F: return radray::gpu::TextureFormat::RGBA16_FLOAT;
        case RN_PIXEL_RGBA32F: return radray::gpu::TextureFormat::RGBA32_FLOAT;
        case RN_PIXEL_DEPTH32F: return radray::gpu::TextureFormat::D32_FLOAT;
        case RN_PIXEL_R8_SNORM: return radray::gpu::TextureFormat::R8_SNORM;
        case RN_PIXEL_RG8_UNORM: return radray::gpu::TextureFormat::RG8_UNORM;
        case RN_PIXEL_RG8_SNORM: return radray::gpu::TextureFormat::RG8_SNORM;
        case RN_PIXEL_RGBA8_SNORM: return radray::gpu::TextureFormat::RGBA8_SNORM;
        case RN_PIXEL_R16_UNORM: return radray::gpu::TextureFormat::R16_UNORM;
        case RN_PIXEL_R16_SNORM: return radray::gpu::TextureFormat::R16_SNORM;
        case RN_PIXEL_RG16_UNORM: return radray::gpu::TextureFormat::RG16_UNORM;
        case RN_PIXEL_RG16_SNORM: return radray::gpu::TextureFormat::RG16_SNORM;
        case RN_PIXEL_RGBA16_UNORM: return radray::gpu::TextureFormat::RGBA16_UNORM;
        case RN_PIXEL_RGBA16_SNORM: return radray::gpu::TextureFormat::RGBA16_SNORM;
        case RN_PIXEL_R8_UINT: return radray::gpu::TextureFormat::R8_UINT;
        case RN_PIXEL_R8_SINT: return radray::gpu::TextureFormat::R8_SINT;
        case RN_PIXEL_RG8_UINT: return radray::gpu::TextureFormat::RG8_UINT;
        case RN_PIXEL_RG8_SINT: return radray::gpu::TextureFormat::RG8_SINT;
        case RN_PIXEL_RGBA8_UINT: return radray::gpu::TextureFormat::RGBA8_UINT;
        case RN_PIXEL_RGBA8_SINT: return radray::gpu::TextureFormat::RGBA8_SINT;
        case RN_PIXEL_R16_UINT: return radray::gpu::TextureFormat::R16_UINT;
        case RN_PIXEL_R16_SINT: return radray::gpu::TextureFormat::R16_SINT;
        case RN_PIXEL_RG16_UINT: return radray::gpu::TextureFormat::RG16_UINT;
        case RN_PIXEL_RG16_SINT: return radray::gpu::TextureFormat::RG16_SINT;
        case RN_PIXEL_RGBA16_UINT: return radray::gpu::TextureFormat::RGBA16_UINT;
        case RN_PIXEL_RGBA16_SINT: return radray::gpu::TextureFormat::RGBA16_SINT;
        case RN_PIXEL_R32_UINT: return radray::gpu::TextureFormat::R32_UINT;
        case RN_PIXEL_R32_SINT: return radray::gpu::TextureFormat::R32_SINT;
        case RN_PIXEL_RG32_UINT: return radray::gpu::TextureFormat::RG32_UINT;
        case RN_PIXEL_RG32_SINT: return radray::gpu::TextureFormat::RG32_SINT;
        case RN_PIXEL_RGBA32_UINT: return radray::gpu::TextureFormat::RGBA32_UINT;
        case RN_PIXEL_RGBA32_SINT: return radray::gpu::TextureFormat::RGBA32_SINT;
        case RN_PIXEL_R16_FLOAT: return radray::gpu::TextureFormat::R16_FLOAT;
        case RN_PIXEL_RG16_FLOAT: return radray::gpu::TextureFormat::RG16_FLOAT;
        case RN_PIXEL_R32_FLOAT: return radray::gpu::TextureFormat::R32_FLOAT;
        case RN_PIXEL_RG32_FLOAT: return radray::gpu::TextureFormat::RG32_FLOAT;
        case RN_PIXEL_RGB10A2_UNORM: return radray::gpu::TextureFormat::RGB10A2_UNORM;
        case RN_PIXEL_RGB10A2_UINT: return radray::gpu::TextureFormat::RGB10A2_UINT;
        case RN_PIXEL_RG11B10_FLOAT: return radray::gpu::TextureFormat::RG11B10_FLOAT;
        case RN_PIXEL_D32_FLOAT_S8_UINT: return radray::gpu::TextureFormat::D32_FLOAT_S8_UINT;
        case RN_PIXEL_D24_UNORM_S8_UINT: return radray::gpu::TextureFormat::D24_UNORM_S8_UINT;
        case RN_PIXEL_D16_UNORM: return radray::gpu::TextureFormat::D16_UNORM;
        default: return radray::gpu::TextureFormat::UNKNOWN;
    }
}

radray::IntrusivePtr<radray::GpuTexture> CreateSceneTexture(radray::gpu::Device& device, const rn_texture_desc& desc) {
    const auto format = ToTextureFormat(desc.format);
    auto dimension = desc.array_layers == 1 ? radray::gpu::TextureDimension::Dim2D : radray::gpu::TextureDimension::Dim2DArray;
    if (desc.flags & RN_TEXTURE_CUBE) dimension = desc.array_layers == 6 ? radray::gpu::TextureDimension::Cube : radray::gpu::TextureDimension::CubeArray;
    auto usage = radray::gpu::TextureUse::CopyDestination | radray::gpu::TextureUse::Resource;
    if (desc.flags & RN_TEXTURE_READBACK) usage |= radray::gpu::TextureUse::CopySource;
    if (desc.flags & RN_TEXTURE_ATTACHMENT) {
        if (radray::gpu::IsDepthStencilFormat(format))
            usage |= radray::gpu::TextureUse::DepthStencilRead | radray::gpu::TextureUse::DepthStencilWrite;
        else
            usage |= radray::gpu::TextureUse::RenderTarget;
    }
    return radray::GpuTexture::Create(device, {
                                                  .Dim = dimension,
                                                  .Width = desc.width,
                                                  .Height = desc.height,
                                                  .ArrayLayers = desc.array_layers,
                                                  .MipLevels = desc.mip_levels,
                                                  .Format = format,
                                                  .Usage = usage,
                                              })
        .Release();
}

radray::gpu::TextureView* GetSamplingView(radray::gpu::Device& device, radray::GpuTexture& owner, const rn_texture_sampling& sampling) {
    auto* texture = owner.GetTexture();
    const auto& desc = texture->GetDesc();
    if (sampling.base_mip >= desc.MipLevels || sampling.mip_count > desc.MipLevels - sampling.base_mip) {
        RADRAY_ERR_LOG("Sampling view mip range exceeds texture");
        return nullptr;
    }
    const uint32_t count = sampling.mip_count ? sampling.mip_count : desc.MipLevels - sampling.base_mip;
    if (!sampling.base_mip && count == desc.MipLevels) {
        if (auto view = owner.GetDefaultView()) return view.Get();
    }
    return owner.GetOrCreateView(device, {
                                             .Dim = desc.Dim,
                                             .Format = desc.Format,
                                             .Range = {0, desc.DepthOrArraySize, sampling.base_mip, count, radray::gpu::IsDepthStencilFormat(desc.Format) ? radray::gpu::TextureAspect::Depth : radray::gpu::TextureAspect::Color},
                                             .Usage = radray::gpu::TextureViewUsage::Resource,
                                         })
        .Get();
}

radray::gpu::Sampler* GetTextureSampler(radray::gpu::Device& device, uint32_t bits, const rn_texture_sampling& sampling) {
    if ((bits & ~63u) || !std::isfinite(sampling.max_lod) || sampling.max_lod < 0 || !sampling.max_anisotropy ||
        sampling.max_anisotropy > device.GetCapabilities().Limits.MaxSamplerAnisotropy) {
        RADRAY_ERR_LOG("Invalid texture sampler LOD or anisotropy");
        return nullptr;
    }
    // Minecraft's Vulkan sampler keeps a quarter-level range even when mipmaps are disabled.
    const float lod = std::max(0.25f, (bits & 32) ? 0.f : sampling.max_lod);
    return device.GetOrCreateSampler({
                                         .AddressS = (bits & 1) ? radray::gpu::AddressMode::Repeat : radray::gpu::AddressMode::ClampToEdge,
                                         .AddressT = (bits & 2) ? radray::gpu::AddressMode::Repeat : radray::gpu::AddressMode::ClampToEdge,
                                         .AddressR = radray::gpu::AddressMode::ClampToEdge,
                                         .MinFilter = (bits & 4) ? radray::gpu::FilterMode::Linear : radray::gpu::FilterMode::Nearest,
                                         .MagFilter = (bits & 8) ? radray::gpu::FilterMode::Linear : radray::gpu::FilterMode::Nearest,
                                         .MipmapFilter = (bits & 16) ? radray::gpu::FilterMode::Linear : radray::gpu::FilterMode::Nearest,
                                         .LodMax = lod,
                                         .AnisotropyClamp = sampling.max_anisotropy,
                                     })
        .Get();
}

FrameTextureImports::FrameTextureImports(
    radray::RDGBuilder& graph,
    std::pmr::memory_resource& scratch,
    const radray::gpu::DeviceLimits* limits)
    : _graph(graph), _limits(limits), _cleared(&scratch) {}

radray::RDGTextureHandle FrameTextureImports::Import(const rn_texture* texture, bool overwrite) {
    return Import(texture->Texture, overwrite);
}

std::pmr::memory_resource* FrameTextureImports::Scratch() const noexcept {
    return _cleared.get_allocator().resource();
}

radray::RDGTextureHandle FrameTextureImports::Import(
    const radray::IntrusivePtr<radray::GpuTexture>& texture,
    bool overwritesWholeSubresource) {
    const auto handle = _graph.ImportPersistentOwned(texture);
    const auto& desc = texture->GetDesc();
    if (_limits) {
        if (texture->GetSubmittedState() != radray::gpu::TextureState::Undefined || !_cleared.insert(texture.Get()).second ||
            (overwritesWholeSubresource && desc.ArrayLayers == 1 && desc.MipLevels == 1)) {
            return handle;
        }
        auto zero = +[](const int&, radray::RDGUploadWriteContext& ctx) noexcept {
            std::memset(ctx.Bytes.data(), 0, ctx.Bytes.size());
            return true;
        };
        const auto aspects = radray::gpu::GetTextureFormatAspects(desc.Format);
        for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
            const uint32_t width = std::max(1u, desc.Width >> mip);
            const uint32_t height = std::max(1u, desc.Height >> mip);
            const uint64_t bytes = uint64_t(width) * radray::gpu::GetTextureFormatBytesPerPixel(desc.Format);
            for (uint32_t layer = 0; layer < desc.ArrayLayers; ++layer) {
                const radray::RDGAttachmentSlice slice{mip, layer, 1};
                if (desc.Usage & radray::gpu::TextureUse::RenderTarget) {
                    _graph.AddClearColor(handle, {{0, 0, 0, 0}}, slice);
                } else if (desc.Usage & radray::gpu::TextureUse::DepthStencilWrite) {
                    _graph.AddClearDepthStencil(handle, {0, 0}, slice, true, bool(aspects & radray::gpu::TextureAspect::Stencil));
                } else if (!radray::gpu::IsDepthStencilFormat(desc.Format)) {
                    _graph.UploadTexture(handle, {mip, layer, 0, 0, width, height}, radray::RDGTextureUploadSource::Generated(bytes * height, 0, zero));
                } else {
                    const uint64_t align = std::max(4u, _limits->TextureDataPitchAlignment);
                    const uint64_t size = ((bytes + align - 1) / align) * align * height;
                    auto src = _graph.CreateDynamicBuffer(
                        {.MaxSize = size, .Usage = radray::gpu::BufferUse::CopySource, .Alignment = std::max<uint64_t>(4, _limits->TextureDataPlacementAlignment)},
                        radray::RDGUploadSource::Generated(size, 0, zero));
                    struct Fill {
                        radray::RDGTextureHandle Dst;
                        radray::RDGBufferHandle Src;
                        radray::gpu::SubresourceRange Range;
                        radray::gpu::Texture* Image{};
                    };
                    radray::gpu::SubresourceRange range{layer, 1, mip, 1, aspects};
                    for (auto aspect : {radray::gpu::TextureAspect::Depth, radray::gpu::TextureAspect::Stencil}) {
                        if (aspects & aspect) {
                            range.Aspects = aspect;
                            _graph.AddCopyPass(
                                      "Initialize depth plane",
                                      Fill{handle, src, range},
                                      +[](const Fill& fill, radray::RDGCopyPassContext& ctx) noexcept {
                                          auto sliceBuf = ctx.ResolveBuffer(fill.Src);
                                          ctx.GetCommandBuffer().CopyBufferToTexture(fill.Image, fill.Range, sliceBuf.Buffer.Get(), sliceBuf.Offset);
                                          return ctx.GetCommandBuffer().IsValid();
                                      },
                                      +[](Fill& fill, radray::RDGPrepareContext& ctx) noexcept {
                                          fill.Image = &ctx.Resolve(fill.Dst);
                                          return true;
                                      })
                                .ReadBuffer(src)
                                .WriteTexture(handle);
                        }
                    }
                }
            }
        }
        return handle;
    }
    if (texture->GetSubmittedState() == radray::gpu::TextureState::Undefined && desc.Dim == radray::gpu::TextureDimension::Dim2D &&
        desc.MipLevels == 1 && bool(desc.Usage & radray::gpu::TextureUse::RenderTarget) && _cleared.insert(texture.Get()).second) {
        _graph.AddClearColor(handle, {{0, 0, 0, 0}});
    }
    return handle;
}

bool ValidateTextureDesc(const radray::gpu::GpuDeviceCapabilities& caps, const rn_texture_desc& desc) {
    uint32_t levels = 1;
    for (uint32_t side = std::max(desc.width, desc.height); side > 1; side >>= 1) ++levels;
    if (desc.struct_size != sizeof(desc) || desc.reserved || ToTextureFormat(desc.format) == radray::gpu::TextureFormat::UNKNOWN ||
        !desc.width || !desc.height || !desc.array_layers || desc.array_layers > caps.Limits.MaxTextureArrayLayers || (desc.flags & ~7u) ||
        ((desc.flags & RN_TEXTURE_CUBE) && (desc.array_layers % 6 || desc.width != desc.height)) ||
        desc.width > caps.Limits.MaxTexture2DDimension ||
        desc.height > caps.Limits.MaxTexture2DDimension || !desc.mip_levels || desc.mip_levels > levels) {
        RADRAY_ERR_LOG("Invalid texture descriptor: size={}x{} mips={} format={}", desc.width, desc.height, desc.mip_levels, uint32_t(desc.format));
        return false;
    }
    return true;
}

bool PrepareTextureUpload(const rn_texture& handle, const rn_texture_upload_desc& desc, PendingTextureUpload& pending) {
    const auto& texture = handle.Desc;
    if (desc.struct_size != sizeof(desc) || !desc.pixels || desc.array_layer >= texture.array_layers || desc.mip_level >= texture.mip_levels || !desc.width || !desc.height) {
        RADRAY_ERR_LOG("Invalid upload descriptor or missing texture: mip={}", desc.mip_level);
        return false;
    }
    const uint32_t width = std::max(1u, texture.width >> desc.mip_level);
    const uint32_t height = std::max(1u, texture.height >> desc.mip_level);
    if (radray::gpu::IsDepthStencilFormat(ToTextureFormat(texture.format))) {
        RADRAY_ERR_LOG("CPU depth texture upload is unsupported");
        return false;
    }
    const uint64_t rowBytes = uint64_t(desc.width) * radray::gpu::GetTextureFormatBytesPerPixel(ToTextureFormat(texture.format));
    const uint64_t pitch = desc.row_pitch ? desc.row_pitch : rowBytes;
    if (uint64_t(desc.x) + desc.width > width || uint64_t(desc.y) + desc.height > height || pitch < rowBytes ||
        desc.byte_size < pitch * (desc.height - 1) + rowBytes) {
        RADRAY_ERR_LOG(
            "Texture upload region, row pitch or byte size is invalid: mip={} region={},{},{}x{} row_pitch={} byte_size={}",
            desc.mip_level, desc.x, desc.y, desc.width, desc.height, pitch, desc.byte_size);
        return false;
    }
    pending.Target = handle.Texture;
    pending.Desc = desc;
    pending.Desc.texture = nullptr;
    pending.Bytes.resize(size_t(rowBytes * desc.height));
    for (uint32_t row = 0; row < desc.height; ++row) {
        std::memcpy(pending.Bytes.data() + rowBytes * row, static_cast<const radray::byte*>(desc.pixels) + pitch * row, size_t(rowBytes));
    }
    pending.Desc.pixels = nullptr;
    pending.Desc.row_pitch = uint32_t(rowBytes);
    pending.Desc.byte_size = pending.Bytes.size();
    return true;
}

void PendingResourceOps::AddUploads(radray::RDGBuilder& graph, FrameTextureImports& imports, std::span<const ShadowWrite> shadows) {
    for (auto& upload : BufferUploads) {
        auto target = graph.ImportPersistentOwned(upload.Target);
        graph.UploadBuffer(target, 0, radray::RDGUploadSource::OwnedBytes(std::move(upload.Bytes)));
        graph.MarkOutput(target);
    }
    for (auto& write : shadows) {
        auto target = graph.ImportPersistentOwned(write.Target);
        graph.UploadBuffer(target, write.Offset, radray::RDGUploadSource::BorrowedBytes(write.Bytes));
        graph.MarkOutput(target);
    }
    for (auto& upload : TextureUploads) {
        const auto& desc = upload.Target->GetDesc();
        const auto target = imports.Import(upload.Target, upload.Desc.width == desc.Width && upload.Desc.height == desc.Height);
        const radray::gpu::TextureRegion2D region{upload.Desc.mip_level, upload.Desc.array_layer, upload.Desc.x, upload.Desc.y, upload.Desc.width, upload.Desc.height};
        graph.UploadTexture(target, region, radray::RDGTextureUploadSource::OwnedBytes(std::move(upload.Bytes), upload.Desc.row_pitch));
        graph.MarkOutput(target);
    }
}

void PendingResourceOps::Clear() noexcept {
    TextureUploads.clear();
    BufferUploads.clear();
    RemovedModelMeshes.clear();
}
}  // namespace raynium
