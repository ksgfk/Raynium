#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/modular_frame_pipeline.h>
#include <radray/runtime/frame_scheduler.h>
#include <raynium/immediate_pipeline.h>
#include <raynium/static_geometry_registry.h>
#include <raynium/batch_envelope.h>
#include <span>
#include <utility>

namespace raynium {
struct FrameExecution {
    uint64_t sequence{};
    rn_present_outcome outcome{RN_PRESENT_NONE};
    uint32_t flags{};
    uint64_t wait_ns{}, prepare_ns{}, rdg_ns{}, present_ns{};
    rn_frame_metrics metrics{};
};
class Renderer final {
public:
    ~Renderer() noexcept;
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,uint32_t flags,radray::unique_ptr<Renderer>&);
    RenderProgramLibrary& Programs() { return _pipeline->Programs(); }
    ModularFramePipeline& Pipeline() { return *_pipeline; }
    BufferPool& Buffers() { return _buffers; }
    bool SubmitFrame(rn_renderer*,BatchEnvelope&,FrameExecution&,const std::function<void(uint64_t)>& submitted, const std::function<bool()>& collect);
    bool IsTerminal() const { return _poisoned || _device.GetDevice().IsDeviceLost(); }
    void Finish();
    bool UpsertModelMesh(int32_t meshId,std::span<const radray::byte> vertices,uint32_t vertexCount,uint32_t indexCount,uint32_t& pageId) {
        return !IsTerminal() && _staticGeometry->Upsert(meshId,vertices,vertexCount,indexCount,pageId);
    }
    bool RemoveModelMesh(int32_t meshId) { return _staticGeometry->Remove(meshId); }
    bool SupportsWireframe() const { return _pipeline->Resources().SupportsWireframe(); }
    radray::DeviceContext& Runtime() { return _device; }
private:
    Renderer(radray::DeviceContext& device,radray::unique_ptr<radray::FrameScheduler> scheduler,
        radray::unique_ptr<radray::VertexBufferPool> vertices,radray::unique_ptr<radray::VertexBufferPool> translucentVertices,
        radray::unique_ptr<radray::IndexBufferPool> quadPool,radray::unique_ptr<radray::IndexBufferPool> indices,
        radray::unique_ptr<StaticGeometryRegistry> staticGeometry,ModularFramePipeline* pipeline)
        :_device(device),_scheduler(std::move(scheduler)),
         _terrain(device,std::move(vertices),std::move(translucentVertices),std::move(quadPool),std::move(indices)),
         _staticGeometry(std::move(staticGeometry)),_pipeline(pipeline) {}
    radray::DeviceContext& _device;
    radray::unique_ptr<radray::FrameScheduler> _scheduler;
    TerrainResidency _terrain;
    radray::unique_ptr<StaticGeometryRegistry> _staticGeometry;
    ModularFramePipeline* _pipeline;
    BufferPool _buffers;
    bool _poisoned{};
};
}
