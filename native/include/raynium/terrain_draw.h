#pragma once
#include <raynium/bridge.h>
#include <radray/render/rhi.h>
#include <array>
#include <span>
#include <vector>

namespace raynium {
struct MeshMaterialBinding;
struct TerrainDrawRange { uint32_t first{},count{}; int32_t vertex{}; };
struct TerrainDrawTemplate {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    std::array<TerrainDrawRange,32> ranges{};
    uint32_t fullMask{},validMask{},mergeMask{};
};
struct TerrainVisible {
    const TerrainDrawTemplate* geometry{};
    const MeshMaterialBinding* material{};
    // State in the high 32 bits, visibility/depth key in the low 32 bits.
    uint64_t sortKey{};
    uint32_t mask{},firstInstance{},instanceCount{};
};
struct TerrainDrawBatch {
    radray::render::VertexBufferView vertices{};
    radray::render::IndexBufferView indices{};
    const MeshMaterialBinding* material{};
    uint32_t first{},count{};
};
struct TerrainDrawLayer { uint32_t firstBatch{},batchCount{},firstRecord{},recordCount{}; uint64_t selectedGroups{},indicesInstances{}; };
struct TerrainFrameLists {
    std::array<std::vector<TerrainVisible>,3> visible;
    std::vector<TerrainVisible> sortScratch;
    std::vector<radray::render::DrawIndexedIndirectArguments> arguments;
    std::vector<TerrainDrawBatch> batches;
    std::array<TerrainDrawLayer,3> layers;
};
// Inputs retain their relative order for equal state/depth keys. Multi-instance items never merge face groups.
bool BuildTerrainDraws(TerrainFrameLists&,bool oit,radray::string& error);
uint32_t TerrainStateSortKey(const TerrainDrawTemplate&,const MeshMaterialBinding&) noexcept;
// Returns the next native-call record count; zero means unsupported or no remaining records.
uint32_t TerrainIndirectChunk(uint32_t remaining,uint32_t nativeLimit) noexcept;
// Successful auto fallback returns its reason; forced unsupported modes fail before resource creation.
bool SelectTerrainSubmission(uint32_t flags,const radray::render::RenderDeviceCapabilities&,bool& indirect,radray::string& reason);
}
