#include <raynium/timestamp_queries.h>
#include <radray/logger.h>
#include <limits>

namespace raynium {
using namespace radray::gpu;
bool FrameTimestamps::Prepare(Device& device,CommandQueue& queue,std::span<const TimestampTarget> writes) {
    if(_sequence) RADRAY_ABORT("Cannot reuse in-flight timestamp storage");
    _writes.assign(writes.begin(),writes.end());
    if(writes.empty()) return true;
    const auto bits=queue.GetTimestampValidBits();
    if(!bits || bits>64 || !device.GetCapabilities().Features.TimestampQueries || writes.size()>UINT32_MAX) return false;
    _mask=std::numeric_limits<uint64_t>::max()>>(64-bits);
    const auto count=uint32_t(writes.size());
    if(!_storage || _storage->queries->GetCount()<count) {
        const uint64_t size=uint64_t(count)*sizeof(uint64_t);
        auto pool=device.CreateQueryPool({QueryType::Timestamp,count,"Raynium frame timestamps"}).Release();
        if(!pool) return false;
        auto buffer=device.CreateBuffer({.Size=size,.Memory=MemoryType::HostReadBack,.Usage=BufferUse::CopyDestination|BufferUse::MapRead}).Release();
        if(!buffer) return false;
        auto* mapped=static_cast<const uint64_t*>(buffer->GetMappedData().Get());
        if(!mapped) return false;
        _storage=radray::make_unique<Storage>(std::move(pool),std::move(buffer),mapped);
        _state=BufferState::Undefined;
    }
    return true;
}
void FrameTimestamps::Reset(radray::RDGBuilder& graph) const {
    if(_writes.empty()) return;
    struct Reset { QueryPool* pool; uint32_t count; };
    auto pass=graph.AddCopyPass("Reset user timestamps",Reset{_storage->queries.get(),uint32_t(_writes.size())},
        +[](const Reset& reset,radray::RDGCopyPassContext& ctx) noexcept {
            ctx.GetCommandBuffer().ResetQueryPool(reset.pool,0,reset.count);
            return true;
        }).NeverCull().GetHandle();
    graph.AddOrderBarrier(pass);
}
void FrameTimestamps::Write(radray::RDGBuilder& graph,uint32_t physicalIndex) const {
    auto pass=graph.AddCopyPass("Write user timestamp",QueryTimestampDescriptor{_storage->queries.get(),QueryPipelineStage::Bottom,physicalIndex},
        +[](const QueryTimestampDescriptor& write,radray::RDGCopyPassContext& ctx) noexcept {
            ctx.GetCommandBuffer().WriteTimestamp(write);
            return true;
        }).NeverCull().GetHandle();
    graph.AddOrderBarrier(pass);
}
void FrameTimestamps::WriteInline(CommandBuffer& commands,uint32_t index) const {
    commands.WriteTimestamp({_storage->queries.get(),QueryPipelineStage::Bottom,index});
}
void FrameTimestamps::Resolve(radray::RDGBuilder& graph) const {
    if(_writes.empty()) return;
    struct Resolve { QueryPool* pool; uint32_t count; radray::RDGBufferHandle buffer; };
    auto buffer=graph.ImportBuffer(_storage->readback.get(),_state,BufferState::HostRead);
    auto pass=graph.AddCopyPass("Resolve user timestamps",Resolve{_storage->queries.get(),uint32_t(_writes.size()),buffer},
        +[](const Resolve& data,radray::RDGCopyPassContext& ctx) noexcept {
            auto slice=ctx.ResolveBuffer(data.buffer);
            ctx.GetCommandBuffer().ResolveQueryData({data.pool,0,data.count,slice.Buffer.Get(),slice.Offset});
            return true;
        }).WriteBuffer(buffer).NeverCull().GetHandle();
    graph.AddOrderBarrier(pass);
}
void FrameTimestamps::Submitted(uint64_t sequence) {
    if(_writes.empty()) return;
    _sequence=sequence;
    _state=BufferState::HostRead;
}
bool FrameTimestamps::Collect(uint64_t completed,radray::vector<PublishedTimestamp>& output) {
    if(!_sequence || _sequence>completed) return true;
    if(!_storage->readback->InvalidateMappedRange({0,_writes.size()*sizeof(uint64_t)})) return false;
    // The last write to a logical slot wins, including when multiple writes share one submission.
    for(size_t i=0;i<_writes.size();++i) {
        const auto& write=_writes[i];
        output.push_back({write.pool,write.slot,_sequence,_storage->mapped[i]&_mask});
    }
    _writes.clear();
    _sequence=0;
    return true;
}
}
