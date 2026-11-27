#include <raynium/timestamp_queries.h>

#include <limits>
#include <utility>

#include <radray/logger.h>

namespace raynium {
bool FrameTimestamps::Prepare(
    radray::gpu::Device& device,
    radray::gpu::CommandQueue& queue,
    std::span<const TimestampTarget> writes) {
    if (_sequence) RADRAY_ABORT("Cannot reuse in-flight timestamp storage");
    _writes.assign(writes.begin(), writes.end());
    if (writes.empty()) return true;
    const auto bits = queue.GetTimestampValidBits();
    if (!bits || bits > 64 || !device.GetCapabilities().Features.TimestampQueries || writes.size() > UINT32_MAX) return false;
    _mask = std::numeric_limits<uint64_t>::max() >> (64 - bits);
    const auto count = uint32_t(writes.size());
    if (!_storage || _storage->Queries->GetCount() < count) {
        const uint64_t size = uint64_t(count) * sizeof(uint64_t);
        auto pool = device.CreateQueryPool({radray::gpu::QueryType::Timestamp, count, "Raynium frame timestamps"}).Release();
        if (!pool) return false;
        auto buffer = device.CreateBuffer({
                                              .Size = size,
                                              .Memory = radray::gpu::MemoryType::HostReadBack,
                                              .Usage = radray::gpu::BufferUse::CopyDestination | radray::gpu::BufferUse::MapRead,
                                          })
                          .Release();
        if (!buffer) return false;
        auto* mapped = static_cast<const uint64_t*>(buffer->GetMappedData().Get());
        if (!mapped) return false;
        _storage = radray::make_unique<Storage>(std::move(pool), std::move(buffer), mapped);
        _state = radray::gpu::BufferState::Undefined;
    }
    return true;
}

void FrameTimestamps::Reset(radray::RDGBuilder& graph) const {
    if (_writes.empty()) return;
    struct ResetOp {
        radray::gpu::QueryPool* Pool;
        uint32_t Count;
    };
    auto pass = graph.AddCopyPass(
                         "Reset user timestamps",
                         ResetOp{_storage->Queries.get(), uint32_t(_writes.size())},
                         +[](const ResetOp& reset, radray::RDGCopyPassContext& ctx) noexcept {
                             ctx.GetCommandBuffer().ResetQueryPool(reset.Pool, 0, reset.Count);
                             return true;
                         })
                    .NeverCull()
                    .GetHandle();
    graph.AddOrderBarrier(pass);
}

void FrameTimestamps::Write(radray::RDGBuilder& graph, uint32_t physicalIndex) const {
    auto pass = graph.AddCopyPass(
                         "Write user timestamp",
                         radray::gpu::QueryTimestampDescriptor{_storage->Queries.get(), radray::gpu::QueryPipelineStage::Bottom, physicalIndex},
                         +[](const radray::gpu::QueryTimestampDescriptor& write, radray::RDGCopyPassContext& ctx) noexcept {
                             ctx.GetCommandBuffer().WriteTimestamp(write);
                             return true;
                         })
                    .NeverCull()
                    .GetHandle();
    graph.AddOrderBarrier(pass);
}

void FrameTimestamps::WriteInline(radray::gpu::CommandBuffer& commands, uint32_t index) const {
    commands.WriteTimestamp({_storage->Queries.get(), radray::gpu::QueryPipelineStage::Bottom, index});
}

void FrameTimestamps::Resolve(radray::RDGBuilder& graph) const {
    if (_writes.empty()) return;
    struct ResolveOp {
        radray::gpu::QueryPool* Pool;
        uint32_t Count;
        radray::RDGBufferHandle Buffer;
    };
    auto buffer = graph.ImportBuffer(_storage->Readback.get(), _state, radray::gpu::BufferState::HostRead);
    auto pass = graph.AddCopyPass(
                         "Resolve user timestamps",
                         ResolveOp{_storage->Queries.get(), uint32_t(_writes.size()), buffer},
                         +[](const ResolveOp& data, radray::RDGCopyPassContext& ctx) noexcept {
                             auto slice = ctx.ResolveBuffer(data.Buffer);
                             ctx.GetCommandBuffer().ResolveQueryData({data.Pool, 0, data.Count, slice.Buffer.Get(), slice.Offset});
                             return true;
                         })
                    .WriteBuffer(buffer)
                    .NeverCull()
                    .GetHandle();
    graph.AddOrderBarrier(pass);
}

void FrameTimestamps::Submitted(uint64_t sequence) {
    if (_writes.empty()) return;
    _sequence = sequence;
    _state = radray::gpu::BufferState::HostRead;
}

bool FrameTimestamps::Collect(uint64_t completed, radray::vector<PublishedTimestamp>& output) {
    if (!_sequence || _sequence > completed) return true;
    if (!_storage->Readback->InvalidateMappedRange({0, _writes.size() * sizeof(uint64_t)})) return false;
    // The last write to a logical slot wins, including when multiple writes share one submission.
    for (size_t i = 0; i < _writes.size(); ++i) {
        const auto& write = _writes[i];
        output.push_back({write.Pool, write.Slot, _sequence, _storage->Mapped[i] & _mask});
    }
    _writes.clear();
    _sequence = 0;
    return true;
}
}  // namespace raynium
