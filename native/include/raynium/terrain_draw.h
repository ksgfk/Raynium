#pragma once

#include <cstdint>
#include <span>

#include <radray/gpu/types.h>
#include <radray/types.h>
#include <raynium/bridge.h>

namespace raynium {
struct TerrainDrawRange {
    uint32_t First{};
    uint32_t Count{};
    int32_t Vertex{};
};

struct TerrainMeshLayout {
    radray::vector<TerrainDrawRange> Ranges;
    radray::array<uint32_t, 7> GroupBits{};
    // Three bits per input quad for restoring the order of multi-group solid/cutout meshes.
    radray::vector<uint8_t> OriginalQuadGroups;
    radray::array<double, 3> Minimum{};
    radray::array<double, 3> Maximum{};
    uint32_t VertexCount{};
    rn_material_mode Layer{};
    bool FiniteBounds{};
};

struct TerrainEye {
    radray::array<double, 3> Offset{};
    bool Valid{};
};

bool BuildPreanalyzedTerrainMeshLayout(
    const rn_terrain_mesh_layout& source,
    uint32_t vertexCount,
    rn_material_mode layer,
    TerrainMeshLayout& layout);
bool WriteQuadIndices(std::span<radray::byte> destination, uint32_t vertexCount) noexcept;
bool WriteWireframeIndices(std::span<radray::byte> destination, const TerrainMeshLayout& layout, uint32_t mask) noexcept;
bool WriteLineIndices(std::span<radray::byte> destination, uint32_t vertexCount) noexcept;
TerrainEye ComputeTerrainEye(const rn_terrain_view_desc& frame) noexcept;
uint32_t TerrainVisibleMask(
    const TerrainMeshLayout& layout,
    const TerrainEye& eye,
    const rn_terrain_view_desc& frame,
    std::span<const rn_instance> instances) noexcept;
// Logs the selected mode and any auto fallback; forced unsupported modes fail before resource creation.
bool SelectTerrainSubmission(uint32_t flags, const radray::gpu::GpuDeviceCapabilities& caps, bool& indirect);
}  // namespace raynium
