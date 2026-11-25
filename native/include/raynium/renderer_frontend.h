#pragma once

#include <raynium/batch_envelope.h>
#include <raynium/renderer.h>
#include <raynium/render_channel.h>
#include <mutex>

namespace raynium {

class RendererFrontend final {
public:
    RendererFrontend(rn_renderer* owner,RenderChannel& session,Renderer& renderer);
    ~RendererFrontend();
    bool Submit(const rn_batch_desc&, Swapchain* target, rn_submit_result&);
    bool Wait(uint64_t sequence, int64_t timeoutNs, rn_wait_result&);
    bool Flush(rn_wait_result&);
    void Snapshot(rn_wait_result&);
    bool Open();
    bool PollCapture(uint64_t, rn_capture_info&, void*, uint64_t);
    bool CreateQueryPool(uint32_t count,rn_query_pool*& id);
    bool CreateTexture(const rn_texture_desc&,rn_texture*&);
    bool RemoveTexture(rn_texture* id);
    bool UploadTexture(const rn_texture_upload_desc&);
    bool RegisterPipeline(const rn_pipeline_desc&,rn_pipeline*& id);
    bool RemovePipeline(rn_pipeline* id);
    bool RemoveQueryPool(rn_query_pool* id);
    bool Query(rn_query_pool* id, uint32_t first, std::span<rn_query_sample> samples, rn_wait_result&);
    bool CreateBuffer(const rn_buffer_desc&, rn_buffer_result&);
    bool RemoveBuffer(rn_buffer* id);
    void SurfaceConfigured();
    void SurfaceLost();
    uint32_t Drain(std::span<rn_batch_telemetry>);
    void DeviceFailure();

    void Consume(BatchEnvelope&);
    void RemoveModelMesh(int32_t id) { _pending.removedModelMeshes.push_back(id); }
    void ApplyModelRemovals();
private:
    bool Collect(bool terminal=false);
    bool OnSlotAvailable();
    void OnSlotAvailableTelemetry();
    void SampleProgress();
    void Advance(uint64_t done);
    void SettleDeviceLoss();
    void Present(rn_present_outcome);
    bool FailInput(rn_submit_result&);
    rn_renderer* _owner;
    RenderChannel& _session;
    Renderer& _renderer;
    alignas(64) std::atomic<uint64_t> _consumed{},_submitted{},_presented{};
    alignas(64) std::atomic<uint64_t> _completed{},_ceiling{UINT64_MAX};
    std::atomic<uint32_t> _flags{},_generation{},_recovery{},_outcome{RN_PRESENT_NONE};
    uint64_t _enqueued{};
    std::mutex _publishMutex,_telemetryMutex;
    std::atomic<bool> _hasWrites{};
    struct PublishedWrites { uint64_t sequence; radray::vector<HostBufferWrite> writes; };
    radray::deque<PublishedWrites> _writes;
    radray::deque<rn_batch_telemetry> _telemetry;
    radray::deque<uint64_t> _readbacks;
    radray::array<BatchEnvelope,2> _envelopes;
    PendingResourceOps _pending;
    HandleList<rn_texture> _textures;
    HandleList<rn_buffer> _buffers;
    HandleList<rn_pipeline> _pipelines;
    HandleList<rn_query_pool> _queries;
    struct RetiredPipeline { uint64_t sequence; radray::unique_ptr<ImmediatePipeline> pipeline; };
    radray::deque<RetiredPipeline> _retired;
};
}
