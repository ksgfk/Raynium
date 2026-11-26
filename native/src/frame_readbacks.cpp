#include <raynium/frame_readbacks.h>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
void HostBufferWrite::Apply() const {
    if(!host->alive.load()) return;
    auto* dest=host->bytes.data()+destination;
    if(!bytes.empty()) {
        std::memcpy(dest,bytes.data(),bytes.size());
        return;
    }
    const auto* source=staging->mapped+offset;
    for(uint32_t row=0;row<rows;++row)
        std::memcpy(dest+row*rowBytes,source+row*pitch,rowBytes);
}
void FrameReadbacks::AddInline(rn_buffer* target,uint64_t destination,radray::vector<radray::byte>&& bytes) {
    _entries.push_back({target->readbackHost,destination,bytes.size(),1,bytes.size(),0,std::move(bytes)});
}
bool FrameReadbacks::Reserve(rn_buffer* target,uint64_t destination,uint64_t rowBytes,uint32_t rows,
                            uint64_t pitch,uint64_t alignment,uint64_t& offset) {
    if(!alignment || !rows || !rowBytes || pitch<rowBytes || pitch>UINT64_MAX/rows) return false;
    const uint64_t padding=(alignment-_size%alignment)%alignment;
    if(padding>UINT64_MAX-_size) return false;
    offset=_size+padding;
    if(pitch*rows>UINT64_MAX-offset || !target->Contains(destination,rowBytes*rows)) return false;
    _size=offset+pitch*rows;
    _entries.push_back({target->readbackHost,destination,rowBytes,rows,pitch,offset,{}});
    return true;
}
bool FrameReadbacks::Prepare(Device& device) {
    if(!_size || (_storage && _storage->UseCount()==1 && _storage->buffer->GetDesc().Size>=_size)) return true;
    auto buffer=device.CreateBuffer({.Size=_size,.Memory=MemoryType::HostReadBack,.Usage=BufferUse::CopyDestination|BufferUse::MapRead}).Release();
    if(!buffer) return false;
    auto* mapped=static_cast<const radray::byte*>(buffer->GetMappedData().Get());
    if(!mapped) return false;
    _storage=radray::IntrusivePtr<MappedReadback>(new MappedReadback(std::move(buffer),mapped));
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
bool FrameReadbacks::Collect(uint64_t completed, radray::vector<HostBufferWrite>& output) {
    if(!_sequence || _sequence>completed) return true;
    if(_size && !_storage->buffer->InvalidateMappedRange({0,_size})) return false;
    for(auto& entry:_entries) {
        if(entry.bytes.empty()) entry.staging=_storage;
        output.push_back(std::move(entry));
    }
    _entries.clear();
    _size=0;
    _sequence=0;
    return true;
}
}
