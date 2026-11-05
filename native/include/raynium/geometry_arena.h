#pragma once
#include <raynium/render_resources.h>
#include <radray/runtime/vertex_input.h>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace raynium {
// Vertex storage shared by every resident mesh of one scene. Blocks stay resident for the arena
// lifetime; only vertex ranges are recycled.
// Lifetime rules and the retirement boundary: doc/architecture/native-runtime.md.
class GeometryArena;

// Borrowed vertex range. The owning arena must outlive every allocation of it.
class GeometryAllocation final {
public:
    // Empty range; no arena access and no release.
    GeometryAllocation() noexcept = default;
    GeometryAllocation(const GeometryAllocation&) = delete;
    GeometryAllocation& operator=(const GeometryAllocation&) = delete;
    GeometryAllocation(GeometryAllocation&& other) noexcept;
    GeometryAllocation& operator=(GeometryAllocation&& other) noexcept;
    ~GeometryAllocation() noexcept; // Returns the range to its arena.
    const radray::VertexInput& Input() const noexcept; // Shared by every mesh of the block.
    radray::render::Buffer* VertexBuffer() const noexcept;
    uint32_t FirstVertex() const noexcept { return _first; }
    uint32_t VertexCount() const noexcept { return _count; }
    uint64_t ByteOffset() const noexcept { return uint64_t(_first)*VertexStride; }
    uint64_t ByteSize() const noexcept { return uint64_t(_count)*VertexStride; }
    explicit operator bool() const noexcept { return _arena!=nullptr; }
private:
    friend class GeometryArena;
    static constexpr uint32_t VertexStride=28;
    GeometryAllocation(GeometryArena* arena,uint32_t block,uint32_t first,uint32_t count) noexcept
        :_arena(arena),_block(block),_first(first),_count(count) {}
    GeometryArena* _arena{};
    uint32_t _block{};
    uint32_t _first{};
    uint32_t _count{};
};

class GeometryArena final {
public:
    static constexpr uint32_t VertexStride=28;
    static constexpr uint32_t BlockVertices=(32u<<20)/VertexStride;
    static bool Create(radray::render::Device&,radray::unique_ptr<GeometryArena>&,radray::string&);
    ~GeometryArena() noexcept;
    GeometryArena(const GeometryArena&) = delete;
    GeometryArena& operator=(const GeometryArena&) = delete;
    // Best fit over free ranges; creates another block when none fits.
    std::optional<GeometryAllocation> Allocate(uint32_t vertexCount,radray::string&);
    size_t BlockCount() const noexcept;
    uint64_t FreeBytes() const noexcept;
private:
    friend class GeometryAllocation;
    struct Block;
    explicit GeometryArena(radray::render::Device&);
    void Release(uint32_t block,uint32_t first,uint32_t count) noexcept;
    bool AddBlock(uint32_t vertices,radray::string&);
    radray::render::Device& _device;
    // Blocks are heap-allocated so each block VertexInput keeps a stable address.
    std::vector<radray::unique_ptr<Block>> _blocks;
    std::set<std::tuple<uint32_t,uint32_t,uint32_t>> _bySize; // (vertex count, block, first vertex)
};
}
