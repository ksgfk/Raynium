#include <raynium/geometry_arena.h>
#include <radray/logger.h>
#include <algorithm>
#include <iterator>

namespace raynium {
using namespace radray::render;
struct GeometryArena::Block {
    RenderBuffer buffer;
    radray::VertexInput input;
    std::map<uint32_t,uint32_t> free; // first vertex -> vertex count, merged on release
    uint32_t capacity{};
};
GeometryAllocation::GeometryAllocation(GeometryAllocation&& other) noexcept
    :_arena(other._arena),_block(other._block),_first(other._first),_count(other._count) {
    other._arena=nullptr; other._block=other._first=other._count=0;
}
GeometryAllocation& GeometryAllocation::operator=(GeometryAllocation&& other) noexcept {
    if(this!=&other) {
        if(_arena) _arena->Release(_block,_first,_count);
        _arena=other._arena; _block=other._block; _first=other._first; _count=other._count;
        other._arena=nullptr; other._block=other._first=other._count=0;
    }
    return *this;
}
GeometryAllocation::~GeometryAllocation() noexcept { if(_arena) _arena->Release(_block,_first,_count); }
const radray::VertexInput& GeometryAllocation::Input() const noexcept { return _arena->_blocks[_block]->input; }
Buffer* GeometryAllocation::VertexBuffer() const noexcept { return _arena->_blocks[_block]->buffer.get(); }

bool GeometryArena::Create(Device& device,radray::unique_ptr<GeometryArena>& output,radray::string& error) {
    if(output) { error="Geometry arena output occupied"; return false; }
    auto arena=radray::unique_ptr<GeometryArena>(new GeometryArena(device));
    if(!arena->AddBlock(BlockVertices,error)) return false;
    output=std::move(arena); return true;
}
GeometryArena::~GeometryArena() noexcept {
#ifdef RADRAY_IS_DEBUG
    for(size_t index=0;index<_blocks.size();++index) {
        const auto& block=*_blocks[index];
        // Every allocation must be released before the arena: Debug builds fail on a leaked range.
        RADRAY_ASSERT(!block.free.empty() && block.free.size()==1 && block.free.begin()->first==0 &&
            block.free.begin()->second==block.capacity);
        (void)block;
    }
#endif
}
size_t GeometryArena::BlockCount() const noexcept { return _blocks.size(); }
GeometryArena::GeometryArena(Device& device) :_device(device) {}
uint64_t GeometryArena::FreeBytes() const noexcept {    uint64_t bytes=0;
    for(const auto& entry:_bySize) bytes+=uint64_t(std::get<0>(entry))*VertexStride;
    return bytes;
}
std::optional<GeometryAllocation> GeometryArena::Allocate(uint32_t vertexCount,radray::string& error) {
    if(!vertexCount) { error="Empty terrain geometry allocation"; return std::nullopt; }
    if(const auto found=_bySize.lower_bound({vertexCount,0,0}); found!=_bySize.end()) {
        const auto [count,index,first]=*found;
        _bySize.erase(found);
        auto& free=_blocks[index]->free;
        free.erase(first);
        if(count>vertexCount) {
            const uint32_t rest=first+vertexCount;
            free.emplace(rest,count-vertexCount);
            _bySize.emplace(count-vertexCount,index,rest);
        }
        return GeometryAllocation(this,index,first,vertexCount);
    }
    // A single mesh larger than a regular block gets its own block; the current ABI cannot reach this.
    const uint32_t vertices=std::max(BlockVertices,vertexCount);
    if(!AddBlock(vertices,error)) return std::nullopt;
    const auto index=static_cast<uint32_t>(_blocks.size()-1);
    auto& free=_blocks[index]->free;
    free.erase(0);
    _bySize.erase({vertices,index,0});
    if(vertices>vertexCount) {
        free.emplace(vertexCount,vertices-vertexCount);
        _bySize.emplace(vertices-vertexCount,index,vertexCount);
    }
    return GeometryAllocation(this,index,0,vertexCount);
}
void GeometryArena::Release(uint32_t index,uint32_t first,uint32_t count) noexcept {
    auto& free=_blocks[index]->free;
    uint32_t start=first, length=count;
    auto next=free.lower_bound(first);
    if(next!=free.end() && start+length==next->first) {
        _bySize.erase({next->second,index,next->first});
        length+=next->second;
        next=free.erase(next);
    }
    if(next!=free.begin()) {
        const auto previous=std::prev(next);
        if(previous->first+previous->second==start) {
            _bySize.erase({previous->second,index,previous->first});
            start=previous->first;
            length+=previous->second;
            free.erase(previous);
        }
    }
    free.emplace(start,length);
    _bySize.emplace(length,index,start);
}
bool GeometryArena::AddBlock(uint32_t vertices,radray::string& error) {
    auto block=radray::make_unique<Block>();
    const uint64_t bytes=uint64_t(vertices)*VertexStride;
    block->buffer=CreateBuffer(_device,bytes,MemoryType::Device,BufferUse::CopyDestination|BufferUse::Vertex);
    if(!block->buffer) { error="Cannot create terrain vertex block"; return false; }
    block->capacity=vertices;
    block->input.Layouts={{0,VertexStride,VertexStepMode::Vertex},{1,16,VertexStepMode::Instance}};
    block->input.Attributes={{0,0,"POSITION",0,VertexFormat::FLOAT32X3,0},{0,12,"COLOR",0,VertexFormat::UNORM8X4,1},
        {0,16,"TEXCOORD",0,VertexFormat::FLOAT32X2,2},{0,24,"TEXCOORD",1,VertexFormat::UINT16X2,3},
        {1,0,"TEXCOORD",2,VertexFormat::SINT32X3,4},{1,12,"TEXCOORD",3,VertexFormat::FLOAT32,5}};
    block->input.Streams={{block->buffer.get(),0,bytes,0}};
    block->free.emplace(0,vertices);
    _bySize.emplace(vertices,static_cast<uint32_t>(_blocks.size()),0);
    _blocks.push_back(std::move(block));
    return true;
}
}
