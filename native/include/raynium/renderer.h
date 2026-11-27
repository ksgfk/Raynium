#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>
#include <radray/types.h>
#include <raynium/batch_envelope.h>
#include <raynium/immediate_pipeline.h>
#include <raynium/modular_frame_pipeline.h>
#include <raynium/static_geometry_registry.h>

namespace raynium {
struct FrameExecution {
    uint64_t Sequence{};
    rn_present_outcome Outcome{RN_PRESENT_NONE};
    uint32_t Flags{};
    uint64_t WaitNs{};
    uint64_t PrepareNs{};
    uint64_t RdgNs{};
    uint64_t PresentNs{};
    rn_frame_metrics Metrics{};
};

class Renderer final {
public:
    ~Renderer() noexcept;
    static bool Create(
        radray::DeviceContext& device,
        std::span<const radray::byte> shaders,
        uint32_t flags,
        radray::unique_ptr<Renderer>& out);
    RenderProgramLibrary& Programs() noexcept;
    ModularFramePipeline& Pipeline() noexcept;
    BufferPool& Buffers() noexcept;
    bool SubmitFrame(
        rn_renderer* owner,
        BatchEnvelope& envelope,
        FrameExecution& result,
        const std::function<void(uint64_t)>& notifySubmitted,
        const std::function<bool()>& collect);
    bool IsTerminal() const noexcept;
    void Finish();
    bool UpsertModelMesh(
        int32_t meshId,
        std::span<const radray::byte> vertices,
        uint32_t vertexCount,
        uint32_t indexCount,
        uint32_t& pageId);
    bool RemoveModelMesh(int32_t meshId);
    bool SupportsWireframe() const noexcept;
    radray::DeviceContext& Runtime() noexcept;

private:
    Renderer(
        radray::DeviceContext& device,
        radray::unique_ptr<radray::FrameScheduler> scheduler,
        radray::unique_ptr<radray::VertexBufferPool> vertices,
        radray::unique_ptr<radray::VertexBufferPool> translucentVertices,
        radray::unique_ptr<radray::IndexBufferPool> quadPool,
        radray::unique_ptr<radray::IndexBufferPool> indices,
        radray::unique_ptr<StaticGeometryRegistry> staticGeometry,
        ModularFramePipeline* pipeline);

    radray::DeviceContext& _device;
    radray::unique_ptr<radray::FrameScheduler> _scheduler;
    TerrainResidency _terrain;
    radray::unique_ptr<StaticGeometryRegistry> _staticGeometry;
    ModularFramePipeline* _pipeline{};
    BufferPool _buffers;
    bool _poisoned{};
};
}  // namespace raynium
