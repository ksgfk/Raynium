#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>
#include <type_traits>

#include <radray/runtime/rdg.h>
#include <radray/runtime/readback_queue.h>
#include <raynium/bridge.h>
#include <raynium/frame_readbacks.h>
#include <raynium/immediate_pass.h>
#include <raynium/resource_uploads.h>
#include <raynium/timestamp_queries.h>

namespace raynium {
class OutputPassResources;

class FrameTapePlan {
public:
    void Measure(rn_frame_metrics& metrics) const;
    void Replay(
        radray::RDGBuilder& graph,
        FrameTextureImports& imports,
        radray::AsyncReadbackQueue& queue,
        const OutputPassResources& output,
        const FrameTimestamps& timestamps,
        const FrameReadbacks& readbacks,
        radray::RDGTextureHandle backBuffer);

protected:
    struct Region {
        const rn_texture* Texture{};
        radray::gpu::TextureRegion2D Area{};
        bool Full{};
    };

    struct Clear {
        Region Target;
        radray::gpu::ColorClearValue Value{};
    };

    struct Capture {
        Region Source;
        uint64_t Id{};
    };

    struct Upload {
        Region Target;
        std::span<const radray::byte> Pixels;
        uint32_t Pitch{};
    };

    struct WriteBuffer {
        rn_buffer* Target{};
        uint64_t Offset{};
        std::span<const radray::byte> Borrowed;
    };

    struct CopyBuffer {
        rn_buffer* Source{};
        rn_buffer* Destination{};
        uint64_t SrcOffset{};
        uint64_t DstOffset{};
        uint64_t Size{};
        uint64_t StagingOffset{};
    };

    struct TextureBuffer {
        Region Image;
        rn_buffer* Buffer{};
        uint64_t Offset{};
        uint64_t Pitch{};
        uint64_t StagingOffset{};
        uint32_t AlignedPitch{};
        bool ToImage{};
        bool Direct{};
    };

    struct Timestamp {
        uint32_t PhysicalIndex{};
    };

    struct Blit {
        Region Source;
    };

    struct Copy {
        Region Source;
        Region Destination;
    };

    using TapeOp = FrameOp<Clear, Capture, Upload, Copy, Blit, Timestamp, WriteBuffer, CopyBuffer, TextureBuffer, RasterPassPlan>;

    void ReplayTextureBuffer(
        radray::RDGBuilder& graph,
        FrameTextureImports& imports,
        const FrameReadbacks& readbacks,
        const TextureBuffer& op);

    std::span<TerrainView> _views;
    std::span<GeometryUpload> _worldUploads;
    std::span<const radray::byte> _instanceUploads;
    std::span<radray::gpu::DrawIndexedIndirectArguments> _geometryIndirectArgs;
    radray::gpu::GpuBackend _backend{};
    std::span<TapeOp> _commands;
};
static_assert(std::is_trivially_destructible_v<FrameTapePlan> && std::is_trivially_destructible_v<RasterPassPlan>);

class TapeCompiler final : public FrameTapePlan {
public:
    explicit TapeCompiler(radray::FrameSlot& slot);
    bool PrepareViews(
        const rn_batch_desc& batch,
        TerrainResidency& terrain,
        rn_renderer* owner,
        const MeshPassResources& mesh,
        bool indirect);
    bool Compile(
        const rn_batch_desc& batch,
        rn_renderer* owner,
        const StaticGeometryRegistry& staticGeometry,
        const TerrainResidency& terrain,
        FrameReadbacks& frame,
        const std::function<bool(uint64_t, uint32_t, uint32_t)>& reserveCapture,
        FrameTimestamps& timestamps);
    std::pmr::vector<GeometryUpload>& WorldUploads() noexcept;
    void FinishUploads() noexcept;

private:
    radray::FrameSlot& _slot;
    std::pmr::vector<TerrainView> _viewStorage;
    std::pmr::vector<GeometryUpload> _uploads;
    std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments> _args;
    std::pmr::vector<TimestampTarget> _timestamps;
};
}  // namespace raynium
