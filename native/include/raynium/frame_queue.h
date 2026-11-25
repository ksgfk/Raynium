#pragma once

#include <raynium/frame_packet.h>
#include <raynium/renderer.h>
#include <raynium/render_thread_session.h>
#include <mutex>

namespace raynium {

class FrameQueue final {
public:
    FrameQueue(RenderThreadSession& session, Renderer& renderer);
    ~FrameQueue();
    bool Submit(const rn_batch_desc&, Swapchain* target, rn_submit_result&);
    bool Wait(uint64_t sequence, int64_t timeoutNs, rn_wait_result&);
    bool Flush(rn_wait_result&);
    void Snapshot(rn_wait_result&);
    bool Open();
    bool PollCapture(uint64_t, rn_capture_info&, void*, uint64_t);
    bool CreateQueryPool(uint32_t count, uint32_t& id);
    bool CreateTexture(const rn_uploaded_texture_desc&);
    bool RemoveTexture(uint32_t id);
    bool UploadTexture(const rn_texture_upload_desc&);
    bool RegisterPipeline(const rn_pipeline_desc&,uint32_t& id);
    bool RemovePipeline(uint32_t id);
    bool RemoveQueryPool(uint32_t id);
    bool Query(uint32_t id, uint32_t first, std::span<rn_query_sample> samples, rn_wait_result&);
    bool CreateBuffer(const rn_buffer_desc&, rn_buffer_result&);
    bool RemoveBuffer(uint32_t id);
    void SurfaceConfigured();
    void SurfaceLost();
    uint32_t Drain(std::span<rn_batch_telemetry>);
    void DeviceFailure();

private:
    struct PendingReadback { uint64_t sequence; radray::vector<uint64_t> captures; };
    struct QuerySlot { uint64_t sequence{},ticks{}; bool available{}; };
    struct Capture { uint64_t sequence; rn_capture_info info{}; radray::vector<radray::byte> pixels; };
    struct FrontBuffer {
        uint32_t usage;
        uint64_t size;
        // A returned mapping stays valid until explicit release, including a failed FIFO registration.
        radray::IntrusivePtr<radray::GpuUploadBuffer> upload;
        radray::vector<radray::byte> host;
    };
    bool Collect(bool terminal = false);
    bool Poll();
    bool OnSlotAvailable();
    void PublishTelemetryLocked();
    void TryAdvanceFrontendProgress();
    void AdvanceFrontendLocked(uint64_t hardwareDone);
    void RefreshCompletedLocked(uint64_t hardwareDone);
    void SettleDeviceLoss();
    void SnapshotLocked(rn_wait_result&);
    void Present(rn_present_outcome);
    void FailInput(rn_submit_result&);

    RenderThreadSession& _session;
    Renderer& _renderer;
    std::mutex _mutex;
    rn_wait_result _progress{};
    uint64_t _enqueued{};
    uint64_t _maxBufferSize;
    uint32_t _nextBuffer{1}, _nextQueryPool{1}, _nextPipeline{1};
    bool _timestamps;
    radray::unordered_map<uint32_t,rn_uploaded_texture_desc> _textures;
    radray::unordered_set<uint32_t> _pipelines;
    radray::unordered_set<uint32_t> _usedTextureIds;
    bool _queued{}, _closing{}, _collectScheduled{};
    radray::unordered_map<uint64_t,Capture> _captures;
    radray::unordered_map<uint32_t,radray::vector<QuerySlot>> _queries;
    radray::unordered_map<uint32_t,FrontBuffer> _buffers;
    radray::vector<HostBufferWrite> _writes;
    radray::deque<rn_batch_telemetry> _telemetry;
    // The worker owns payloads. Frontend fence sampling reads the first sequence under _mutex.
    radray::deque<PendingReadback> _readbackBatches;
};
}
