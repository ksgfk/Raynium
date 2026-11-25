#pragma once

#include <raynium/bridge.h>
#include <raynium/timestamp_queries.h>
#include <raynium/frame_readbacks.h>
#include <raynium/immediate_pass.h>
#include <raynium/texture_registry.h>
#include <radray/runtime/readback_queue.h>
#include <variant>

namespace raynium {
class OutputPassResources;
class CommandTape final {
public:
    void Measure(rn_frame_metrics&) const;
    bool PrepareViews(const rn_batch_desc&,TerrainResidency&,const TextureRegistry&,const MeshPassResources&,radray::FrameSlot&,bool indirect);
    bool Decode(const rn_batch_desc&,const TextureRegistry&,const BufferRegistry&,const ImmediatePipelineRegistry&,
        const StaticGeometryRegistry&,const TerrainResidency&,const radray::gpu::GpuDeviceCapabilities&);
    radray::vector<GeometryUpload>& WorldUploads() noexcept { return _worldUploads; }
    bool PrepareReadbacks(radray::gpu::Device&,FrameReadbacks&);
    bool ReserveReadbacks(radray::AsyncReadbackQueue&) const;
    std::span<const TimestampTarget> Timestamps() const { return _timestamps; }
    void Replay(radray::RDGBuilder&,FrameTextureImports&,radray::AsyncReadbackQueue&,const OutputPassResources&,const FrameTimestamps&,const FrameReadbacks&,radray::gpu::Texture* backBuffer);
private:
    radray::vector<TerrainView> _views;
    radray::vector<GeometryUpload> _worldUploads;
    std::span<const radray::byte> _instanceUploads;
    radray::vector<radray::gpu::DrawIndexedIndirectArguments> _geometryIndirectArgs;
    radray::gpu::GpuBackend _backend{};
    struct Region {
        radray::IntrusivePtr<radray::GpuTexture> texture;
        radray::gpu::TextureRegion2D area;
        bool full;
    };
    struct Clear { Region target; radray::gpu::ColorClearValue value; };
    struct Capture { Region source; uint64_t id; };
    struct Upload { Region target; std::span<const radray::byte> pixels; uint32_t pitch; };
    struct WriteBuffer { radray::shared_ptr<BufferEntry> target; uint64_t offset; std::span<const radray::byte> borrowed; };
    struct CopyBuffer { radray::shared_ptr<BufferEntry> source,destination; uint64_t srcOffset,dstOffset,size,stagingOffset; };
    struct TextureBuffer {
        Region image;
        radray::shared_ptr<BufferEntry> buffer;
        uint64_t offset,pitch,stagingOffset{};
        uint32_t alignedPitch{};
        bool toImage,direct{};
    };
    void ReplayTextureBuffer(radray::RDGBuilder&,FrameTextureImports&,const FrameReadbacks&,const TextureBuffer&);
    struct Timestamp { uint32_t physicalIndex; };
    radray::vector<TimestampTarget> _timestamps;
    struct Blit { Region source; };
    struct Copy { Region source,destination; };
    radray::vector<std::variant<Clear,Capture,Upload,Copy,Blit,Timestamp,WriteBuffer,CopyBuffer,TextureBuffer,radray::shared_ptr<ImmediatePass>>> _commands;
};
}
