#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

#include <raynium/batch_envelope.h>
#include <raynium/render_channel.h>
#include <raynium/renderer.h>

namespace raynium {
class RendererFrontend final {
public:
    RendererFrontend(rn_renderer* owner, RenderChannel& session, Renderer& renderer);
    ~RendererFrontend();
    bool Submit(const rn_batch_desc& desc, Swapchain* target, rn_submit_result& out);
    bool Wait(uint64_t sequence, int64_t timeoutNs, rn_wait_result& out);
    bool Flush(rn_wait_result& out);
    void Snapshot(rn_wait_result& out);
    bool Open();
    bool PollCapture(uint64_t id, rn_capture_info& info, void* pixels, uint64_t capacity);
    bool CreateQueryPool(uint32_t count, rn_query_pool*& out);
    bool CreateTexture(const rn_texture_desc& desc, rn_texture*& out);
    bool RemoveTexture(rn_texture* texture);
    bool UploadTexture(const rn_texture_upload_desc& desc);
    bool RegisterPipeline(const rn_pipeline_desc& desc, rn_pipeline*& out);
    bool RemovePipeline(rn_pipeline* pipeline);
    bool RemoveQueryPool(rn_query_pool* pool);
    bool Query(rn_query_pool* pool, uint32_t first, std::span<rn_query_sample> samples, rn_wait_result& out);
    bool CreateBuffer(const rn_buffer_desc& desc, rn_buffer_result& out);
    bool RemoveBuffer(rn_buffer* buffer);
    void SurfaceConfigured();
    void SurfaceLost();
    uint32_t Drain(std::span<rn_batch_telemetry> out);
    void DeviceFailure();

    void Consume(BatchEnvelope& envelope);
    void RemoveModelMesh(int32_t id);
    void ApplyModelRemovals();

private:
    struct PublishedWrites {
        uint64_t Sequence{};
        radray::vector<HostBufferWrite> Writes;
    };

    struct RetiredPipeline {
        uint64_t Sequence{};
        radray::unique_ptr<ImmediatePipeline> Pipeline;
    };

    bool Collect(bool terminal = false);
    bool OnSlotAvailable();
    void OnSlotAvailableTelemetry();
    void SampleProgress();
    void Advance(uint64_t done);
    void SettleDeviceLoss();
    void Present(rn_present_outcome outcome);
    bool FailInput(rn_submit_result& out);

    rn_renderer* _owner{};
    RenderChannel& _session;
    Renderer& _renderer;
    alignas(64) std::atomic<uint64_t> _consumed{};
    std::atomic<uint64_t> _submitted{};
    std::atomic<uint64_t> _presented{};
    alignas(64) std::atomic<uint64_t> _completed{};
    std::atomic<uint64_t> _ceiling{UINT64_MAX};
    std::atomic<uint32_t> _flags{};
    std::atomic<uint32_t> _generation{};
    std::atomic<uint32_t> _recovery{};
    std::atomic<uint32_t> _outcome{RN_PRESENT_NONE};
    uint64_t _enqueued{};
    std::mutex _publishMutex;
    std::mutex _telemetryMutex;
    std::atomic<bool> _hasWrites{};
    radray::deque<PublishedWrites> _writes;
    radray::deque<rn_batch_telemetry> _telemetry;
    radray::deque<uint64_t> _readbacks;
    radray::array<BatchEnvelope, 2> _envelopes;
    PendingResourceOps _pending;
    HandleList<rn_texture> _textures;
    HandleList<rn_buffer> _buffers;
    HandleList<rn_pipeline> _pipelines;
    HandleList<rn_query_pool> _queries;
    radray::deque<RetiredPipeline> _retired;
};
}  // namespace raynium
