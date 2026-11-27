#include <raynium/command_tape.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <numeric>
#include <optional>
#include <utility>

#include <radray/logger.h>
#include <raynium/batch_envelope.h>
#include <raynium/output_pass.h>
#include <raynium/raster_records.h>

namespace raynium {
namespace {
enum class Opcode : uint32_t {
    ClearColor = 1,
    ClearDepth,
    CaptureColor,
    UploadTexture,
    CopyTexture,
    BlitToSurface,
    WriteTimestamp,
    WriteBufferBytes,
    CopyBufferBytes,
    BufferToTexture,
    TextureToBuffer,
    BeginPass,
    EndPass,
    Draw,
    RetainedTerrain,
    InstancedGeometry
};

struct Header {
    Opcode Op;
    uint32_t Size;
};

struct WireRegion {
    rn_texture* Id;
    uint32_t Mip;
    uint32_t Layer;
    uint32_t X;
    uint32_t Y;
    uint32_t Width;
    uint32_t Height;
};

struct WireClear {
    Header Head;
    WireRegion Target;
    float Value[4];
};

struct WireCapture {
    Header Head;
    WireRegion Source;
    uint64_t Id;
};

struct WireUpload {
    Header Head;
    WireRegion Target;
    uint32_t Pitch;
    uint32_t Reserved;
    uint64_t Offset;
};

struct WireWriteBuffer {
    Header Head;
    rn_buffer* Id;
    uint64_t Offset;
    uint64_t Size;
    uint64_t UploadOffset;
};

struct WireBlit {
    Header Head;
    rn_texture* Texture;
    uint32_t Mip;
    uint32_t Reserved;
};

struct WireCopy {
    Header Head;
    WireRegion Source;
    WireRegion Destination;
};

static_assert(sizeof(WireClear) == 56 && sizeof(WireCapture) == 48 && sizeof(WireUpload) == 56 && sizeof(WireCopy) == 72);

template <class T>
bool Read(std::span<const radray::byte> bytes, T& record) {
    if (bytes.size() != sizeof(T)) return false;
    std::memcpy(&record, bytes.data(), sizeof(T));
    return true;
}

void CopyBytes(
    radray::RDGBuilder& graph,
    radray::RDGBufferHandle source,
    uint64_t srcOffset,
    radray::RDGBufferHandle destination,
    uint64_t dstOffset,
    uint64_t size) {
    struct Pass {
        radray::RDGBufferHandle Src;
        radray::RDGBufferHandle Dst;
        uint64_t SrcOffset;
        uint64_t DstOffset;
        uint64_t Size;
    };
    graph.AddCopyPass(
             "Raynium buffer copy",
             Pass{source, destination, srcOffset, dstOffset, size},
             +[](const Pass& pass, radray::RDGCopyPassContext& ctx) noexcept {
                 const auto from = ctx.ResolveBuffer(pass.Src);
                 const auto to = ctx.ResolveBuffer(pass.Dst);
                 ctx.GetCommandBuffer().CopyBufferToBuffer(to.Buffer.Get(), to.Offset + pass.DstOffset, from.Buffer.Get(), from.Offset + pass.SrcOffset, pass.Size);
                 return true;
             })
        .ReadBuffer(source)
        .WriteBuffer(destination);
}

void CopyRegion(
    radray::RDGBuilder& graph,
    radray::RDGTextureHandle source,
    const radray::gpu::TextureRegion2D& from,
    radray::RDGTextureHandle destination,
    const radray::gpu::TextureRegion2D& to) {
    struct Pass {
        radray::RDGTextureHandle Source;
        radray::RDGTextureHandle Destination;
        radray::gpu::TextureCopyDescriptor Copy;
    };
    graph.AddCopyPass(
             "Raynium texture copy",
             Pass{
                 source,
                 destination,
                 {
                     .DestinationMipLevel = to.MipLevel,
                     .DestinationArrayLayer = to.ArrayLayer,
                     .DestinationX = to.X,
                     .DestinationY = to.Y,
                     .SourceMipLevel = from.MipLevel,
                     .SourceArrayLayer = from.ArrayLayer,
                     .SourceX = from.X,
                     .SourceY = from.Y,
                     .Width = from.Width,
                     .Height = from.Height,
                 },
             },
             +[](const Pass& pass, radray::RDGCopyPassContext& context) noexcept {
                 context.GetCommandBuffer().CopyTextureToTexture(pass.Copy);
                 return context.GetCommandBuffer().IsValid();
             },
             +[](Pass& pass, radray::RDGPrepareContext& context) noexcept {
                 pass.Copy.Source = &context.Resolve(pass.Source);
                 pass.Copy.Destination = &context.Resolve(pass.Destination);
                 return true;
             })
        .ReadTexture(source)
        .WriteTexture(destination);
}
}  // namespace

TapeCompiler::TapeCompiler(radray::FrameSlot& slot)
    : _slot(slot),
      _viewStorage(&slot.GetMemoryResource()),
      _uploads(&slot.GetMemoryResource()),
      _args(&slot.GetMemoryResource()),
      _timestamps(&slot.GetMemoryResource()) {}

std::pmr::vector<GeometryUpload>& TapeCompiler::WorldUploads() noexcept {
    return _uploads;
}

void TapeCompiler::FinishUploads() noexcept {
    _worldUploads = _uploads;
}

void FrameTapePlan::Measure(rn_frame_metrics& metrics) const {
    for (const auto& view : _views) {
        for (const auto& geometry : view.Uploads) metrics.terrain_upload_bytes += geometry.Source.GetMaxSize();
    }
    for (const auto& geometry : _worldUploads) metrics.scene_upload_bytes += geometry.Source.GetMaxSize();
    metrics.scene_upload_bytes += _instanceUploads.size();
    metrics.scene_upload_bytes += uint64_t(_geometryIndirectArgs.size()) * sizeof(radray::gpu::DrawIndexedIndirectArguments);
    for (const auto& command : _commands) {
        if (const auto* raster = command.Get<RasterPassPlan>())
            raster->Measure(metrics);
        else if (const auto* upload = command.Get<Upload>())
            metrics.texture_upload_bytes += upload->Pixels.size();
        else if (const auto* write = command.Get<WriteBuffer>())
            metrics.scene_upload_bytes += write->Borrowed.size();
    }
}

bool TapeCompiler::PrepareViews(
    const rn_batch_desc& batch,
    TerrainResidency& terrain,
    rn_renderer* owner,
    const MeshPassResources& mesh,
    bool indirect) {
    auto& slot = _slot;
    if (batch.struct_size != sizeof(batch) || batch.view_reserved || (batch.view_count && !batch.views) || batch.view_count > 65536) {
        return false;
    }
    _viewStorage.reserve(batch.view_count);
    for (size_t i = 0; i < batch.view_count; ++i) _viewStorage.emplace_back(slot.GetMemoryResource());
    _views = _viewStorage;
    _backend = slot.GetDevice().GetBackend();
    const auto& caps = slot.GetDevice().GetCapabilities();
    for (size_t i = 0; i < _views.size(); ++i) {
        auto& view = _views[i];
        auto& cache = terrain.View(i);
        view.Draws = &cache.Draws;
        view.Mesh = &mesh;
        view.VertexPool = &terrain.VertexPool();
        view.TranslucentVertexPool = &terrain.TranslucentVertexPool();
        view.QuadIndexPool = &terrain.QuadIndexPool();
        view.TranslucentIndexPool = &terrain.TranslucentIndexPool();
        view.Capabilities = &caps;
        view.UseIndirect = indirect;
        view.IndirectLimit = caps.Limits.MaxDrawIndirectCount;
        if (!terrain.PrepareFrame(owner, batch.views[i], mesh, {slot, view.Uploads, cache, indirect}, view.Frame)) return false;
    }
    return true;
}

bool TapeCompiler::Compile(
    const rn_batch_desc& batch,
    rn_renderer* owner,
    const StaticGeometryRegistry& staticGeometry,
    const TerrainResidency& terrain,
    FrameReadbacks& frame,
    const std::function<bool(uint64_t, uint32_t, uint32_t)>& reserveCapture,
    FrameTimestamps& timestamps) {
    auto& device = _slot.GetDevice();
    const auto& caps = device.GetCapabilities();
    auto& memory = _slot.GetMemoryResource();
    std::pmr::vector<TapeOp> ops(&_slot.GetScratchMemoryResource());
    std::pmr::vector<RasterOp> rasterOps(&_slot.GetScratchMemoryResource());
    auto append = [&]<class T>(T payload) -> decltype(auto) {
        if constexpr (TapeOp::template kInline<T>) {
            ops.emplace_back(payload);
        } else {
            auto* entry = FrameArray<T>(memory, 1).data();
            *entry = payload;
            ops.emplace_back(entry);
            return *entry;
        }
    };
    radray::InlineVector<Clear, 4> pendingClears;
    const auto flushClears = [&](RasterPassPlan* pass = nullptr) {
        for (const auto& clear : pendingClears) {
            const auto& target = clear.Target;
            if (!pass || !pass->TryFoldClear(target.Texture, target.Area.MipLevel, target.Area.ArrayLayer, clear.Value)) {
                append(clear);
            }
        }
        pendingClears.clear();
    };
    const uint32_t maxColors = caps.Limits.MaxColorAttachments;
    const bool canGeometryIndirect = caps.Features.IndirectDraw && caps.Features.IndirectFirstInstance && caps.Limits.MaxDrawIndirectCount >= 2;
    const uint32_t indirectLimit = caps.Limits.MaxDrawIndirectCount;
    if (batch.struct_size != sizeof(batch) || (batch.flags & ~RN_BATCH_MEASURE) || batch.reserved || batch.capture_reserved || batch.query_reserved ||
        (batch.dirty_count && !batch.dirty_ranges) ||
        (batch.command_bytes && !batch.commands) || (batch.upload_bytes && !batch.uploads) ||
        (batch.capture_count && !batch.captures) || batch.capture_count > 4096 || (batch.query_count && !batch.queries) ||
        batch.command_bytes > PTRDIFF_MAX || batch.upload_bytes > PTRDIFF_MAX ||
        (batch.instance_upload_bytes && !batch.instance_uploads) || batch.instance_upload_bytes > PTRDIFF_MAX) {
        RADRAY_ERR_LOG("Invalid command batch descriptor");
        return false;
    }
    const auto resolve = [&](const WireRegion& wire, Region& result) {
        auto* entry = ResolveHandle(owner, wire.Id);
        if (!entry || wire.Mip >= entry->Desc.mip_levels || wire.Layer >= entry->Desc.array_layers) return false;
        const uint32_t width = std::max(1u, entry->Desc.width >> wire.Mip);
        const uint32_t height = std::max(1u, entry->Desc.height >> wire.Mip);
        if (!wire.Width || !wire.Height || uint64_t(wire.X) + wire.Width > width || uint64_t(wire.Y) + wire.Height > height) {
            return false;
        }
        result = {entry, {wire.Mip, wire.Layer, wire.X, wire.Y, wire.Width, wire.Height}, wire.Width == width && wire.Height == height};
        return true;
    };
    std::span commands{static_cast<const radray::byte*>(batch.commands), size_t(batch.command_bytes)};
    const std::span uploads{static_cast<const radray::byte*>(batch.uploads), size_t(batch.upload_bytes)};
    _instanceUploads = {static_cast<const radray::byte*>(batch.instance_uploads), size_t(batch.instance_upload_bytes)};
    bool hasBlit = false;
    uint32_t captures = 0;
    uint32_t queries = 0;
    RasterPassPlan* raster = nullptr;
    while (!commands.empty()) {
        Header header{};
        if (commands.size() < sizeof(header)) return false;
        std::memcpy(&header, commands.data(), sizeof(header));
        if (header.Size < sizeof(header) || header.Size > commands.size() || header.Size % 8) return false;
        const auto bytes = commands.first(header.Size);
        commands = commands.subspan(header.Size);
        if (raster && header.Op != Opcode::RetainedTerrain && header.Op != Opcode::InstancedGeometry &&
            header.Op != Opcode::Draw &&
            header.Op != Opcode::WriteTimestamp && header.Op != Opcode::EndPass) {
            return false;
        }
        // Buffer-only transfers cannot observe or modify a pending texture clear.
        const bool bufferOnly = header.Op == Opcode::WriteBufferBytes || header.Op == Opcode::CopyBufferBytes;
        if (!bufferOnly && header.Op != Opcode::BeginPass && header.Op != Opcode::ClearColor && header.Op != Opcode::ClearDepth) {
            flushClears();
        }
        Region region{};
        switch (header.Op) {
            case Opcode::ClearColor:
            case Opcode::ClearDepth: {
                WireClear wire{};
                if (!Read(bytes, wire) || !resolve(wire.Target, region)) return false;
                const auto& desc = region.Texture->Texture->GetDesc();
                const bool depth = header.Op == Opcode::ClearDepth;
                if (depth != radray::gpu::IsDepthStencilFormat(desc.Format) ||
                    !(desc.Usage & (depth ? radray::gpu::TextureUse::DepthStencilWrite : radray::gpu::TextureUse::RenderTarget))) {
                    return false;
                }
                if (depth && !(wire.Value[0] >= 0 && wire.Value[0] <= 1)) return false;
                radray::gpu::ColorClearValue value{};
                std::copy_n(wire.Value, 4, value.Value.begin());
                if (!region.Full || (radray::gpu::GetTextureFormatAspects(desc.Format) & radray::gpu::TextureAspect::Stencil)) {
                    flushClears();
                    append(Clear{region, value});
                    break;
                }
                const auto previous = std::find_if(pendingClears.begin(), pendingClears.end(), [&](const Clear& clear) {
                    return clear.Target.Texture == region.Texture && clear.Target.Area.MipLevel == region.Area.MipLevel &&
                           clear.Target.Area.ArrayLayer == region.Area.ArrayLayer;
                });
                if (previous != pendingClears.end()) {
                    previous->Value = value;
                } else {
                    if (pendingClears.size() == 4) {
                        append(pendingClears.front());
                        std::move(pendingClears.begin() + 1, pendingClears.end(), pendingClears.begin());
                        pendingClears.pop_back();
                    }
                    pendingClears.push_back({region, value});
                }
                break;
            }
            case Opcode::CaptureColor: {
                WireCapture wire{};
                if (!Read(bytes, wire) || !wire.Id || captures >= batch.capture_count || batch.captures[captures++] != wire.Id ||
                    !resolve(wire.Source, region)) {
                    return false;
                }
                const auto& desc = region.Texture->Texture->GetDesc();
                if (desc.Format != radray::gpu::TextureFormat::RGBA8_UNORM || !(desc.Usage & radray::gpu::TextureUse::CopySource)) {
                    return false;
                }
                if (!reserveCapture(wire.Id, region.Area.Width, region.Area.Height)) return false;
                append(Capture{region, wire.Id});
                break;
            }
            case Opcode::UploadTexture: {
                WireUpload wire{};
                if (!Read(bytes, wire) || !resolve(wire.Target, region)) return false;
                const auto format = region.Texture->Texture->GetDesc().Format;
                if (radray::gpu::IsDepthStencilFormat(format)) return false;
                const uint64_t row = uint64_t(wire.Target.Width) * radray::gpu::GetTextureFormatBytesPerPixel(format);
                const uint64_t size = uint64_t(wire.Pitch) * (wire.Target.Height - 1) + row;
                if (wire.Pitch < row || wire.Offset > uploads.size() || size > uploads.size() - wire.Offset) return false;
                auto pixels = uploads.subspan(wire.Offset, size);
                append(Upload{region, pixels, wire.Pitch});
                break;
            }
            case Opcode::CopyTexture: {
                WireCopy wire{};
                Region destination{};
                if (!Read(bytes, wire) || !resolve(wire.Source, region) || !resolve(wire.Destination, destination)) return false;
                const auto& src = region.Texture->Texture->GetDesc();
                if (!(src.Usage & radray::gpu::TextureUse::CopySource) || src.Format != destination.Texture->Texture->GetDesc().Format ||
                    wire.Source.Width != wire.Destination.Width || wire.Source.Height != wire.Destination.Height ||
                    (radray::gpu::GetTextureFormatAspects(src.Format) & radray::gpu::TextureAspect::Stencil) ||
                    (radray::gpu::IsDepthStencilFormat(src.Format) && !(region.Full && destination.Full))) {
                    return false;
                }
                const auto& a = wire.Source;
                const auto& b = wire.Destination;
                if (a.Id == b.Id && a.Mip == b.Mip && a.Layer == b.Layer && a.X < b.X + b.Width && b.X < a.X + a.Width &&
                    a.Y < b.Y + b.Height && b.Y < a.Y + a.Height) {
                    return false;
                }
                append(Copy{region, destination});
                break;
            }
            case Opcode::BlitToSurface: {
                WireBlit wire{};
                if (!Read(bytes, wire) || !batch.surface || hasBlit) return false;
                auto* entry = ResolveHandle(owner, wire.Texture);
                if (!entry || wire.Mip >= entry->Desc.mip_levels || entry->Desc.array_layers != 1 ||
                    !(entry->Desc.flags & RN_TEXTURE_READBACK) ||
                    radray::gpu::IsDepthStencilFormat(entry->Texture->GetDesc().Format) ||
                    radray::gpu::IsUintFormat(entry->Texture->GetDesc().Format) ||
                    radray::gpu::IsSintFormat(entry->Texture->GetDesc().Format)) {
                    return false;
                }
                hasBlit = true;
                append(Blit{{
                    entry,
                    {wire.Mip, 0, 0, 0, std::max(1u, entry->Desc.width >> wire.Mip), std::max(1u, entry->Desc.height >> wire.Mip)},
                    true,
                }});
                break;
            }
            case Opcode::WriteTimestamp: {
                struct WireTimestamp {
                    Header Head;
                    rn_query_pool* Pool;
                    uint32_t Slot;
                    uint32_t Reserved;
                } wire{};
                if (!Read(bytes, wire) || wire.Reserved || queries >= batch.query_count || _timestamps.size() == UINT32_MAX) return false;
                const auto& declared = batch.queries[queries++];
                if (declared.reserved || declared.pool != wire.Pool || declared.slot != wire.Slot) return false;
                auto* pool = ResolveHandle(owner, wire.Pool);
                if (!pool || wire.Slot >= pool->Storage->Slots.size()) return false;
                if (raster)
                    raster->WriteTimestamp(rasterOps, uint32_t(_timestamps.size()));
                else
                    append(Timestamp{uint32_t(_timestamps.size())});
                _timestamps.push_back({pool->Storage, wire.Slot});
                break;
            }
            case Opcode::WriteBufferBytes: {
                WireWriteBuffer wire{};
                if (!Read(bytes, wire)) return false;
                auto target = ResolveHandle(owner, wire.Id);
                if (!target || !(target->Usage & RN_BUFFER_COPY_DST) || !BufferContainsRange(*target, wire.Offset, wire.Size) ||
                    wire.UploadOffset > uploads.size() || wire.Size > uploads.size() - wire.UploadOffset) {
                    return false;
                }
                auto pixels = uploads.subspan(wire.UploadOffset, wire.Size);
                if (IsReadbackBuffer(*target)) frame.AddInline(target, wire.Offset, {pixels.begin(), pixels.end()});
                append(WriteBuffer{target, wire.Offset, pixels});
                break;
            }
            case Opcode::CopyBufferBytes: {
                struct Wire {
                    Header Head;
                    rn_buffer* Source;
                    rn_buffer* Destination;
                    uint64_t SrcOffset;
                    uint64_t DstOffset;
                    uint64_t Size;
                } wire{};
                if (!Read(bytes, wire)) return false;
                auto src = ResolveHandle(owner, wire.Source);
                auto dst = ResolveHandle(owner, wire.Destination);
                if (!src || !dst || !(src->Usage & RN_BUFFER_COPY_SRC) || !(dst->Usage & RN_BUFFER_COPY_DST) ||
                    !BufferContainsRange(*src, wire.SrcOffset, wire.Size) || !BufferContainsRange(*dst, wire.DstOffset, wire.Size) ||
                    (src == dst && wire.SrcOffset < wire.DstOffset + wire.Size && wire.DstOffset < wire.SrcOffset + wire.Size)) {
                    return false;
                }
                auto& copy = append(CopyBuffer{src, dst, wire.SrcOffset, wire.DstOffset, wire.Size, 0});
                if (IsReadbackBuffer(*dst) &&
                    !frame.Reserve(dst, wire.DstOffset, wire.Size, 1, wire.Size, std::max<uint64_t>(1, caps.Limits.BufferCopyOffsetAlignment), copy.StagingOffset)) {
                    return false;
                }
                break;
            }
            case Opcode::BufferToTexture:
            case Opcode::TextureToBuffer: {
                struct Wire {
                    Header Head;
                    WireRegion Image;
                    rn_buffer* Buffer;
                    uint64_t Offset;
                    uint64_t Pitch;
                } wire{};
                if (!Read(bytes, wire) || !resolve(wire.Image, region)) return false;
                const bool toImage = header.Op == Opcode::BufferToTexture;
                auto buffer = ResolveHandle(owner, wire.Buffer);
                const auto& image = region.Texture->Texture->GetDesc();
                const uint64_t row = uint64_t(region.Area.Width) * radray::gpu::GetTextureFormatBytesPerPixel(image.Format);
                if (!buffer || !(buffer->Usage & (toImage ? RN_BUFFER_COPY_SRC : RN_BUFFER_COPY_DST)) ||
                    (!toImage && !(image.Usage & radray::gpu::TextureUse::CopySource)) || wire.Pitch > UINT32_MAX || wire.Pitch < row ||
                    (!toImage && wire.Pitch != row) || !BufferContainsRange(*buffer, wire.Offset, wire.Pitch * (region.Area.Height - 1) + row) ||
                    (radray::gpu::IsDepthStencilFormat(image.Format) &&
                     (toImage || !region.Full || (radray::gpu::GetTextureFormatAspects(image.Format) & radray::gpu::TextureAspect::Stencil)))) {
                    return false;
                }
                auto& transfer = append(TextureBuffer{region, buffer, wire.Offset, wire.Pitch, 0, 0, toImage, false});
                auto& desc = transfer.Image.Texture->Texture->GetDesc();
                const auto& area = transfer.Image.Area;
                const uint64_t minimum = radray::gpu::IsDepthStencilFormat(desc.Format) ? 4u : 1u;
                const uint64_t texel = radray::gpu::GetTextureFormatBytesPerPixel(desc.Format);
                const uint64_t alignment = std::lcm(texel, std::max<uint64_t>(minimum, caps.Limits.TextureDataPitchAlignment));
                const uint64_t placement = std::lcm(texel, std::max<uint64_t>(minimum, caps.Limits.TextureDataPlacementAlignment));
                const uint64_t pitch = (row + alignment - 1) / alignment * alignment;
                if (pitch > UINT32_MAX) return false;
                transfer.AlignedPitch = uint32_t(pitch);
                transfer.Direct = transfer.Offset % placement == 0 && transfer.Pitch % alignment == 0 &&
                                  BufferContainsRange(*transfer.Buffer, transfer.Offset, transfer.Pitch * area.Height);
                if (IsReadbackBuffer(*transfer.Buffer) &&
                    !frame.Reserve(transfer.Buffer, transfer.Offset, row, area.Height, pitch, placement, transfer.StagingOffset)) {
                    return false;
                }
                break;
            }
            case Opcode::RetainedTerrain:
                if (!raster || !raster->AddTerrain(memory, rasterOps, bytes, _views, owner)) return false;
                break;
            case Opcode::InstancedGeometry:
                if (!raster || !raster->AddInstancedGeometry(memory, rasterOps, bytes, _instanceUploads, _args, canGeometryIndirect, indirectLimit, owner, staticGeometry, terrain)) {
                    return false;
                }
                break;
            case Opcode::Draw:
                if (!raster || !raster->AddDraw(memory, rasterOps, bytes, owner)) return false;
                break;
            case Opcode::BeginPass:
                raster = RasterPassPlan::Decode(memory, bytes, owner, maxColors);
                if (!raster) return false;
                flushClears(raster);
                ops.emplace_back(raster);
                break;
            case Opcode::EndPass:
                if (!raster || bytes.size() != sizeof(Header)) return false;
                raster->Finish(memory, rasterOps);
                rasterOps.clear();
                raster = nullptr;
                break;
            default: return false;
        }
    }
    flushClears();
    _commands = FreezeFrameArray(memory, ops);
    _geometryIndirectArgs = _args;
    return !raster && hasBlit == (batch.surface != nullptr) && captures == batch.capture_count && queries == batch.query_count &&
           frame.Prepare(device) && timestamps.Prepare(device, _slot.GetQueue(), _timestamps);
}

void FrameTapePlan::ReplayTextureBuffer(
    radray::RDGBuilder& graph,
    FrameTextureImports& imports,
    const FrameReadbacks& readbacks,
    const TextureBuffer& op) {
    auto texture = imports.Import(op.Image.Texture, op.ToImage && op.Image.Full);
    const auto& region = op.Image.Area;
    const uint64_t row = uint64_t(region.Width) * radray::gpu::GetTextureFormatBytesPerPixel(op.Image.Texture->Texture->GetDesc().Format);
    const bool host = IsReadbackBuffer(*op.Buffer);
    if (op.ToImage && !op.Direct && IsHostVisibleBuffer(*op.Buffer)) {
        const auto length = (region.Height - 1) * op.Pitch + row;
        graph.UploadTexture(
            texture,
            region,
            radray::RDGTextureUploadSource::BorrowedBytes({op.Buffer->Mapped + op.Offset, size_t(length)}, uint32_t(op.Pitch)));
        graph.MarkOutput(texture);
        return;
    }
    auto real = host ? readbacks.Import(graph) : ImportBuffer(*op.Buffer, graph);
    const bool intermediate = !host && !op.Direct;
    auto buffer = intermediate
                      ? graph.CreateBuffer({
                            .Size = uint64_t(op.AlignedPitch) * region.Height,
                            .Usage = radray::gpu::BufferUse::CopySource | radray::gpu::BufferUse::CopyDestination,
                        })
                      : real;
    const uint64_t offset = host ? op.StagingOffset : intermediate ? 0
                                                                   : op.Offset;
    const uint32_t pitch = host || intermediate ? op.AlignedPitch : uint32_t(op.Pitch);
    if (op.ToImage && intermediate) {
        for (uint32_t y = 0; y < region.Height; ++y) CopyBytes(graph, real, op.Offset + y * op.Pitch, buffer, uint64_t(y) * pitch, row);
    }
    struct Transfer {
        radray::RDGTextureHandle Texture;
        radray::RDGBufferHandle Buffer;
        radray::gpu::TextureRegion2D Region;
        uint64_t Offset;
        uint32_t Pitch;
        bool ToImage;
        bool Depth;
        radray::gpu::Texture* Image{};
    };
    auto pass = graph.AddCopyPass(
        "Raynium texture-buffer copy",
        Transfer{texture, buffer, region, offset, pitch, op.ToImage, radray::gpu::IsDepthStencilFormat(op.Image.Texture->Texture->GetDesc().Format)},
        +[](const Transfer& copy, radray::RDGCopyPassContext& ctx) noexcept {
            const auto slice = ctx.ResolveBuffer(copy.Buffer);
            auto& cmd = ctx.GetCommandBuffer();
            const auto address = slice.Offset + copy.Offset;
            if (copy.ToImage) return cmd.CopyBufferToTextureRegion({slice.Buffer.Get(), copy.Image, {address, copy.Pitch, copy.Region}});
            if (copy.Depth) {
                cmd.CopyTextureToBuffer(
                    slice.Buffer.Get(),
                    address,
                    copy.Image,
                    {copy.Region.ArrayLayer, 1, copy.Region.MipLevel, 1, radray::gpu::TextureAspect::Depth});
                return cmd.IsValid();
            }
            return cmd.CopyTextureToBufferRegion({copy.Image, slice.Buffer.Get(), {copy.Region, address, copy.Pitch}});
        },
        +[](Transfer& copy, radray::RDGPrepareContext& ctx) noexcept {
            copy.Image = &ctx.Resolve(copy.Texture);
            return true;
        });
    if (op.ToImage) {
        pass.ReadBuffer(buffer).WriteTexture(texture);
        graph.MarkOutput(texture);
    } else {
        pass.ReadTexture(texture).WriteBuffer(buffer);
        if (intermediate) {
            for (uint32_t y = 0; y < region.Height; ++y) CopyBytes(graph, buffer, uint64_t(y) * pitch, real, op.Offset + y * op.Pitch, row);
        }
        graph.MarkOutput(real);
    }
}

void FrameTapePlan::Replay(
    radray::RDGBuilder& graph,
    FrameTextureImports& imports,
    radray::AsyncReadbackQueue& queue,
    const OutputPassResources& output,
    const FrameTimestamps& timestamps,
    const FrameReadbacks& readbacks,
    radray::RDGTextureHandle backBuffer) {
    for (auto& upload : _worldUploads) ImportGeometryUpload(upload, graph);

    radray::RDGBufferHandle instanceBuffer;
    if (!_instanceUploads.empty()) {
        instanceBuffer = graph.CreateDynamicBuffer(
            {.MaxSize = _instanceUploads.size(), .Usage = radray::gpu::BufferUse::Vertex, .Residency = StreamResidency(_instanceUploads.size())},
            radray::RDGUploadSource::BorrowedBytes(_instanceUploads));
    }
    radray::RDGBufferHandle geometryIndirectBuffer;
    if (!_geometryIndirectArgs.empty()) {
        const auto argBytes = std::as_bytes(std::span{_geometryIndirectArgs});
        geometryIndirectBuffer = graph.CreateDynamicBuffer(
            {.MaxSize = argBytes.size(), .Usage = radray::gpu::BufferUse::Indirect},
            radray::RDGUploadSource::BorrowedBytes(argBytes));
    }
    for (auto& view : _views) view.Import(graph, _backend);
    for (auto& command : _commands) {
        if (auto* clear = command.Get<Clear>()) {
            const auto& target = clear->Target;
            auto handle = imports.Import(
                target.Texture,
                target.Full && !(radray::gpu::GetTextureFormatAspects(target.Texture->Texture->GetDesc().Format) & radray::gpu::TextureAspect::Stencil));
            const auto& r = target.Area;
            const bool depth = radray::gpu::IsDepthStencilFormat(target.Texture->Texture->GetDesc().Format);
            const radray::RDGAttachmentSlice slice{r.MipLevel, r.ArrayLayer, 1};
            std::optional<radray::Rect> rect;
            if (!target.Full) rect = radray::Rect{int32_t(r.X), int32_t(r.Y), r.Width, r.Height};
            if (depth)
                graph.AddClearDepthStencil(handle, {clear->Value.Value[0], 0}, slice, true, false, rect);
            else
                graph.AddClearColor(handle, clear->Value, slice, rect);
            graph.MarkOutput(handle);
        } else if (auto* read = command.Get<Capture>()) {
            auto handle = imports.Import(read->Source.Texture);
            if (!queue.EnqueueReserved(graph, read->Id, handle, read->Source.Area)) {
                RADRAY_ABORT("Reserved capture readback failed to enqueue");
            }
        } else if (auto* upload = command.Get<Upload>()) {
            auto handle = imports.Import(upload->Target.Texture, upload->Target.Full);
            graph.UploadTexture(handle, upload->Target.Area, radray::RDGTextureUploadSource::BorrowedBytes(upload->Pixels, upload->Pitch));
            graph.MarkOutput(handle);
        } else if (auto* write = command.Get<WriteBuffer>()) {
            if (!IsReadbackBuffer(*write->Target)) {
                auto dst = ImportBuffer(*write->Target, graph);
                graph.UploadBuffer(dst, write->Offset, radray::RDGUploadSource::BorrowedBytes(write->Borrowed));
                graph.MarkOutput(dst);
            }
        } else if (auto* copy = command.Get<CopyBuffer>()) {
            auto src = ImportBuffer(*copy->Source, graph);
            const bool toHost = IsReadbackBuffer(*copy->Destination);
            auto dst = toHost ? readbacks.Import(graph) : ImportBuffer(*copy->Destination, graph);
            if (copy->Source == copy->Destination) {
                auto temp = graph.CreateBuffer({
                    .Size = copy->Size,
                    .Usage = radray::gpu::BufferUse::CopySource | radray::gpu::BufferUse::CopyDestination,
                });
                CopyBytes(graph, src, copy->SrcOffset, temp, 0, copy->Size);
                CopyBytes(graph, temp, 0, dst, copy->DstOffset, copy->Size);
            } else {
                CopyBytes(graph, src, copy->SrcOffset, dst, toHost ? copy->StagingOffset : copy->DstOffset, copy->Size);
            }
            graph.MarkOutput(dst);
        } else if (auto* image = command.Get<TextureBuffer>()) {
            ReplayTextureBuffer(graph, imports, readbacks, *image);
        } else if (auto* raster = command.Get<RasterPassPlan>()) {
            raster->Replay(graph, imports, timestamps, instanceBuffer, geometryIndirectBuffer);
        } else if (auto timestamp = command.Get<Timestamp>()) {
            timestamps.Write(graph, timestamp->PhysicalIndex);
        } else if (auto* blit = command.Get<Blit>()) {
            if (backBuffer.IsValid()) {
                const auto& r = blit->Source.Area;
                const auto source = imports.Import(blit->Source.Texture);
                const auto desc = graph.GetTextureDescriptor(backBuffer);
                output.AddPass(graph, source, backBuffer, std::min(r.Width, desc->Width), std::min(r.Height, desc->Height), r.MipLevel, true);
            }
        } else {
            const auto& imageCopy = *command.Get<Copy>();
            auto src = imports.Import(imageCopy.Source.Texture);
            auto dst = imports.Import(imageCopy.Destination.Texture, imageCopy.Destination.Full);
            const auto& r = imageCopy.Source.Area;
            if (imageCopy.Source.Texture == imageCopy.Destination.Texture) {
                // RDG tracks copy states for the entire resource; take a snapshot before switching it to destination.
                auto temp = graph.CreateTexture({
                    .Dim = radray::gpu::TextureDimension::Dim2D,
                    .Width = r.Width,
                    .Height = r.Height,
                    .DepthOrArraySize = 1,
                    .MipLevels = 1,
                    .SampleCount = 1,
                    .Format = imageCopy.Source.Texture->Texture->GetDesc().Format,
                    .Usage = radray::gpu::TextureUse::CopySource | radray::gpu::TextureUse::CopyDestination,
                });
                const radray::gpu::TextureRegion2D origin{0, 0, 0, 0, r.Width, r.Height};
                CopyRegion(graph, src, r, temp, origin);
                CopyRegion(graph, temp, origin, dst, imageCopy.Destination.Area);
            } else {
                CopyRegion(graph, src, r, dst, imageCopy.Destination.Area);
            }
            graph.MarkOutput(dst);
        }
    }
}
}  // namespace raynium
