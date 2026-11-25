#pragma once
#include <radray/types.h>
#include <raynium/bridge.h>
#include <radray/gpu/types.h>
#include <span>
#include <memory_resource>

namespace raynium {
struct TerrainDrawRange { uint32_t first{},count{}; int32_t vertex{}; };
struct TerrainMeshLayout {
    radray::vector<TerrainDrawRange> ranges;
    radray::array<uint32_t,7> groupBits{};
    // Three bits per input quad for restoring the order of multi-group solid/cutout meshes.
    radray::vector<uint8_t> originalQuadGroups;
    radray::array<double,3> minimum{},maximum{};
    uint32_t vertexCount{};
    rn_material_mode layer{};
    bool finiteBounds{};
};
struct TerrainEye { radray::array<double,3> offset{}; bool valid{}; };
bool BuildPreanalyzedTerrainMeshLayout(const rn_terrain_mesh_layout&,uint32_t vertexCount,rn_material_mode,TerrainMeshLayout&);
bool WriteQuadIndices(std::span<radray::byte>,uint32_t vertexCount) noexcept;
bool WriteWireframeIndices(std::span<radray::byte>,const TerrainMeshLayout&,uint32_t mask) noexcept;
bool WriteLineIndices(std::span<radray::byte>,uint32_t vertexCount) noexcept;
TerrainEye ComputeTerrainEye(const rn_terrain_view_desc&) noexcept;
uint32_t TerrainVisibleMask(const TerrainMeshLayout&,const TerrainEye&,const rn_terrain_view_desc&,
                            std::span<const rn_instance>) noexcept;
// Logs the selected mode and any auto fallback; forced unsupported modes fail before resource creation.
bool SelectTerrainSubmission(uint32_t flags,const radray::gpu::GpuDeviceCapabilities&,bool& indirect);
}
