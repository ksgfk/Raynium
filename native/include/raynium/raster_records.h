#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <radray/types.h>
#include <radray/viewport.h>
#include <raynium/bridge.h>

namespace raynium::wire {
struct Pass {
    uint32_t Opcode{};
    uint32_t Size{};
    uint32_t Colors{};
    uint32_t Depth{};
    radray::Rect Area{};
};

struct Draw {
    uint32_t Opcode{};
    uint32_t Size{};
    rn_pipeline* Pipeline{};
    uint32_t Kind{};
    uint32_t Count{};
    uint32_t Instances{};
    uint32_t First{};
    uint32_t FirstInstance{};
    int32_t Base{};
    radray::Rect Scissor{};
    uint32_t Bindings{};
    uint32_t Push{};
};

struct Geometry {
    uint32_t Opcode{};
    uint32_t Size{};
    rn_pipeline* Pipeline{};
    uint32_t Bindings{};
    uint32_t RunCount{};
    uint32_t InstanceCount{};
    uint32_t InstanceStride{};
    uint32_t Push{};
    uint32_t Reserved{};
    uint64_t UploadOffset{};
    radray::Rect Scissor{};
};

struct TerrainMaterial {
    rn_texture* Id{};
    uint32_t Flags{};
    uint32_t Reserved{};
    rn_texture_sampling Sampling{};
};

struct Terrain {
    uint32_t Opcode{};
    uint32_t Size{};
    uint32_t View{};
    uint32_t Stage{};
    radray::Rect Scissor{};
    rn_texture* Ids[3]{};
    uint32_t Mips[3]{};
    uint32_t Reserved{};
    TerrainMaterial Materials[2]{};
};

static_assert(sizeof(Pass) == 32 && sizeof(Draw) == 64 && sizeof(Geometry) == 64 && sizeof(Terrain) == 136);

constexpr bool ValidWireHeader(const Pass& header, size_t length) noexcept {
    return header.Depth <= 1 && length == sizeof(Pass) + (uint64_t(header.Colors) + header.Depth) * 40 &&
           header.Area.X >= 0 && header.Area.Y >= 0 && header.Area.Width && header.Area.Height;
}

constexpr bool ValidWireHeader(const Draw& header, size_t length) noexcept {
    return header.Kind <= 4 && header.Bindings <= 256 && !(header.Push % 4) &&
           length == sizeof(Draw) + uint64_t(header.Bindings) * 48 + ((uint64_t(header.Push) + 7) & ~uint64_t(7));
}

constexpr bool ValidWireHeader(const Geometry& header, size_t length) noexcept {
    return !header.Reserved && header.InstanceStride && header.RunCount && header.InstanceCount &&
           header.Bindings <= 256 && !(header.Push % 4) &&
           length == sizeof(Geometry) + uint64_t(header.Bindings) * 48 + uint64_t(header.RunCount) * 16 +
                         ((uint64_t(header.Push) + 7) & ~uint64_t(7));
}

template <class T>
bool ReadRasterHeader(std::span<const radray::byte> bytes, T& header) {
    if (bytes.size() < sizeof(T)) return false;
    std::memcpy(&header, bytes.data(), sizeof(T));
    return ValidWireHeader(header, bytes.size());
}
}  // namespace raynium::wire
