#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/modular_frame_pipeline.h>
#include <radray/runtime/frame_scheduler.h>
#include <raynium/immediate_pipeline.h>
#include <span>
#include <utility>

namespace raynium {
class Renderer final {
public:
    ~Renderer() noexcept;
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,uint32_t flags,radray::unique_ptr<Renderer>&);
    bool RegisterPipeline(const rn_pipeline_desc& desc,uint32_t& id) { return !IsTerminal() && _pipelines.Register(_device.GetDevice(),_pipeline->Programs(),desc,id,_device.GetPipelineCache()); }
    bool DestroyPipeline(uint32_t id) { return _pipelines.Destroy(id); }
    bool CreateBuffer(const rn_buffer_desc& desc,rn_buffer_result& result) { return !IsTerminal() && _buffers.Create(_device.GetDevice(),desc,result); }
    bool DestroyBuffer(uint32_t id) { return _buffers.Destroy(id); }
    bool CreateQueryPool(uint32_t count,uint32_t& id);
    bool DestroyQueryPool(uint32_t id) { return _queries.Destroy(id); }
    bool SyncQueries(uint32_t pool,uint32_t first,std::span<rn_query_sample> samples,rn_wait_result&);
    bool SubmitFrame(const rn_batch_desc&,Swapchain*,rn_submit_result&);
    bool IsTerminal() const { return _poisoned || _device.GetDevice().IsDeviceLost(); }
    bool PrepareOutput(radray::gpu::TextureFormat format) {
        return _pipeline->PrepareOutput(format,_device.GetDevice());
    }
    void Finish();
    bool WaitSubmit(uint64_t sequence,int64_t timeoutNs,rn_wait_result& result);
    bool CreateTexture(const rn_uploaded_texture_desc& desc) { return _uploaded.Create(_device,desc); }
    bool UploadTexture(const rn_texture_upload_desc& desc) { return _uploaded.Upload(desc); }
    bool DestroyTexture(uint32_t id) { return _uploaded.Destroy(id); }
    bool PollCapture(uint64_t sequence,rn_capture_info& info,void* pixels,uint64_t capacity) { return _pipeline->PollCapture(sequence,info,pixels,capacity); }
    bool SupportsWireframe() const { return _pipeline->Resources().SupportsWireframe(); }
    radray::DeviceContext& Runtime() { return _device; }
private:
    Renderer(radray::DeviceContext& device,radray::unique_ptr<radray::FrameScheduler> scheduler,
        radray::unique_ptr<radray::VertexBufferPool> vertices,radray::unique_ptr<radray::IndexBufferPool> indices,ModularFramePipeline* pipeline)
        :_device(device),_scheduler(std::move(scheduler)),_terrain(device,std::move(vertices),std::move(indices)),_pipeline(pipeline) {}
    radray::DeviceContext& _device;
    radray::unique_ptr<radray::FrameScheduler> _scheduler;
    TerrainResidency _terrain;
    ModularFramePipeline* _pipeline;
    TextureRegistry _uploaded;
    QueryRegistry _queries;
    BufferRegistry _buffers;
    ImmediatePipelineRegistry _pipelines;
    bool _poisoned{};
};
}
