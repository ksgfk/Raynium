#pragma once

#include <raynium/bridge.h>
#include <raynium/timestamp_queries.h>
#include <raynium/frame_readbacks.h>
#include <raynium/immediate_pass.h>
#include <raynium/resource_uploads.h>
#include <radray/runtime/readback_queue.h>
#include <radray/runtime/rdg.h>
#include <functional>

namespace raynium {
class OutputPassResources;
class FrameTapePlan {
public:
    void Measure(rn_frame_metrics&) const;
    void Replay(radray::RDGBuilder&,FrameTextureImports&,radray::AsyncReadbackQueue&,const OutputPassResources&,const FrameTimestamps&,const FrameReadbacks&,radray::RDGTextureHandle backBuffer);
protected:
    std::span<TerrainView> _views;
    std::span<GeometryUpload> _worldUploads;
    std::span<const radray::byte> _instanceUploads;
    std::span<radray::gpu::DrawIndexedIndirectArguments> _geometryIndirectArgs;
    radray::gpu::GpuBackend _backend{};
    struct Region {
        const rn_texture* texture;
        radray::gpu::TextureRegion2D area;
        bool full;
    };
    struct Clear { Region target; radray::gpu::ColorClearValue value; };
    struct Capture { Region source; uint64_t id; };
    struct Upload { Region target; std::span<const radray::byte> pixels; uint32_t pitch; };
    struct WriteBuffer { rn_buffer* target; uint64_t offset; std::span<const radray::byte> borrowed; };
    struct CopyBuffer { rn_buffer *source,*destination; uint64_t srcOffset,dstOffset,size,stagingOffset; };
    struct TextureBuffer {
        Region image;
        rn_buffer* buffer;
        uint64_t offset,pitch,stagingOffset{};
        uint32_t alignedPitch{};
        bool toImage,direct{};
    };
    void ReplayTextureBuffer(radray::RDGBuilder&,FrameTextureImports&,const FrameReadbacks&,const TextureBuffer&);
    struct Timestamp { uint32_t physicalIndex; };
    struct Blit { Region source; };
    struct Copy { Region source,destination; };
    using TapeOp=FrameOp<Clear,Capture,Upload,Copy,Blit,Timestamp,WriteBuffer,CopyBuffer,TextureBuffer,RasterPassPlan>;
    std::span<TapeOp> _commands;
};
static_assert(std::is_trivially_destructible_v<FrameTapePlan> && std::is_trivially_destructible_v<RasterPassPlan>);

class TapeCompiler final : public FrameTapePlan {
public:
    explicit TapeCompiler(radray::FrameSlot& slot):_slot(slot),_viewStorage(&slot.GetMemoryResource()),
        _uploads(&slot.GetMemoryResource()),_args(&slot.GetMemoryResource()),_timestamps(&slot.GetMemoryResource()) {}
    bool PrepareViews(const rn_batch_desc&,TerrainResidency&,rn_renderer*,const MeshPassResources&,bool indirect);
    bool Compile(const rn_batch_desc&,rn_renderer*,const StaticGeometryRegistry&,const TerrainResidency&,
        FrameReadbacks&,const std::function<bool(uint64_t,uint32_t,uint32_t)>& reserveCapture,FrameTimestamps&);
    std::pmr::vector<GeometryUpload>& WorldUploads() { return _uploads; }
    void FinishUploads() { _worldUploads=_uploads; }
private:
    radray::FrameSlot& _slot;
    std::pmr::vector<TerrainView> _viewStorage;
    std::pmr::vector<GeometryUpload> _uploads;
    std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments> _args;
    std::pmr::vector<TimestampTarget> _timestamps;
};
}
