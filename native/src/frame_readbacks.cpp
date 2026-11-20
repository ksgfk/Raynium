#include <raynium/frame_readbacks.h>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
void FrameReadbacks::AddInline(const radray::shared_ptr<BufferEntry>& target,uint64_t destination,radray::vector<radray::byte>&& bytes) {
    _entries.push_back({target,destination,bytes.size(),1,bytes.size(),0,std::move(bytes)});
}
bool FrameReadbacks::Reserve(const radray::shared_ptr<BufferEntry>& target,uint64_t destination,uint64_t rowBytes,uint32_t rows,
                            uint64_t pitch,uint64_t alignment,uint64_t& offset) {
    if(!alignment || !rows || !rowBytes || pitch<rowBytes || pitch>UINT64_MAX/rows) return false;
    const uint64_t padding=(alignment-_size%alignment)%alignment;
    if(padding>UINT64_MAX-_size) return false;
    offset=_size+padding;
    if(pitch*rows>UINT64_MAX-offset || !target->Contains(destination,rowBytes*rows)) return false;
    _size=offset+pitch*rows;
    _entries.push_back({target,destination,rowBytes,rows,pitch,offset,{}});
    return true;
}
bool FrameReadbacks::Prepare(Device& device) {
    if(!_size || (_storage && _storage->buffer->GetDesc().Size>=_size)) return true;
    auto buffer=device.CreateBuffer({.Size=_size,.Memory=MemoryType::ReadBack,.Usage=BufferUse::CopyDestination|BufferUse::MapRead,
        .Hints=ResourceHint::PersistentMap}).Release();
    if(!buffer) return false;
    auto* mapped=static_cast<const radray::byte*>(buffer->Map(0,_size));
    if(!mapped) return false;
    _storage=radray::make_unique<Storage>(std::move(buffer),mapped);
    _state=BufferState::Undefined;
    return true;
}
radray::RDGBufferHandle FrameReadbacks::Import(radray::RDGBuilder& graph) const {
    return graph.ImportBuffer(_storage->buffer.get(),_state,BufferState::HostRead);
}
void FrameReadbacks::Submitted(uint64_t sequence) {
    if(!_entries.empty()) _sequence=sequence;
    if(_size) _state=BufferState::HostRead;
}
void FrameReadbacks::Publish(uint64_t completed) {
    if(!_sequence || _sequence>completed) return;
    if(_size) _storage->buffer->InvalidateMappedRange({0,_size});
    for(const auto& entry:_entries) {
        const auto* source=entry.bytes.empty()?_storage->mapped+entry.offset:entry.bytes.data();
        auto* destination=entry.target->mapped+entry.destination;
        for(uint32_t row=0;row<entry.rows;++row)
            std::memcpy(destination+row*entry.rowBytes,source+row*entry.pitch,entry.rowBytes);
    }
    _entries.clear();
    _size=0;
    _sequence=0;
}
}
