#include <raynium/terrain_draw.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include <Eigen/LU>
#include <radray/logger.h>
#include <radray/types.h>

namespace raynium {
bool SelectTerrainSubmission(uint32_t flags, const radray::gpu::GpuDeviceCapabilities& caps, bool& indirect) {
    indirect = false;
    if (flags > RN_RENDERER_INDIRECT) {
        RADRAY_ERR_LOG("Unknown or mutually exclusive terrain submission flags: {}", flags);
        return false;
    }
    const std::string_view reason = !caps.Features.IndirectDraw             ? "indirect draws unsupported"
                                    : !caps.Features.IndirectFirstInstance  ? "nonzero indirect firstInstance unsupported"
                                    : caps.Limits.MaxDrawIndirectCount <= 1 ? "native multi-record indirect unsupported"
                                                                            : "";
    indirect = flags != RN_RENDERER_DIRECT && reason.empty();
    if (flags == RN_RENDERER_INDIRECT && !indirect) {
        RADRAY_ERR_LOG("Terrain indirect submission unavailable: {}", reason);
        return false;
    }
    RADRAY_INFO_LOG(
        "Terrain submission requested={} actual={} indirect={} first_instance={} max_native_records={} fallback={}",
        flags == RN_RENDERER_DIRECT ? "direct" : flags == RN_RENDERER_INDIRECT ? "indirect"
                                                                               : "auto",
        indirect ? "indirect" : "direct",
        caps.Features.IndirectDraw,
        caps.Features.IndirectFirstInstance,
        caps.Limits.MaxDrawIndirectCount,
        flags == 0 && !indirect ? reason : std::string_view{"none"});
    return true;
}

bool BuildPreanalyzedTerrainMeshLayout(
    const rn_terrain_mesh_layout& source,
    uint32_t vertexCount,
    rn_material_mode layer,
    TerrainMeshLayout& layout) {
    if (layer > RN_MATERIAL_TRANSLUCENT || vertexCount % 4 || vertexCount > 1048576 || source.reserved ||
        (source.flags & ~(RN_TERRAIN_LAYOUT_FINITE_BOUNDS | RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS))) {
        RADRAY_ERR_LOG("Invalid preanalyzed terrain mesh layout header");
        return false;
    }
    const uint64_t expectedQuads = vertexCount / 4;
    uint64_t totalQuads = 0;
    for (uint32_t g = 0; g < 7; ++g) {
        if (layer == RN_MATERIAL_TRANSLUCENT && g < 6 && source.group_quad_counts[g] != 0) {
            RADRAY_ERR_LOG("Preanalyzed translucent mesh cannot use directional groups");
            return false;
        }
        totalQuads += source.group_quad_counts[g];
    }
    if (totalQuads != expectedQuads) {
        RADRAY_ERR_LOG("Preanalyzed terrain quad count mismatch: sum={} expected={}", totalQuads, expectedQuads);
        return false;
    }
    layout = {};
    layout.VertexCount = vertexCount;
    layout.Layer = layer;
    layout.Minimum.fill(std::numeric_limits<double>::infinity());
    layout.Maximum.fill(-std::numeric_limits<double>::infinity());
    const bool finiteFlag = (source.flags & RN_TERRAIN_LAYOUT_FINITE_BOUNDS) != 0;
    if (finiteFlag) {
        if (!vertexCount) {
            RADRAY_ERR_LOG("Preanalyzed empty terrain mesh cannot declare finite bounds");
            return false;
        }
        for (uint32_t a = 0; a < 3; ++a) {
            if (!std::isfinite(source.minimum[a]) || !std::isfinite(source.maximum[a]) || source.minimum[a] > source.maximum[a]) {
                RADRAY_ERR_LOG("Preanalyzed terrain mesh bounds are invalid");
                return false;
            }
            layout.Minimum[a] = source.minimum[a];
            layout.Maximum[a] = source.maximum[a];
        }
        layout.FiniteBounds = true;
    }
    uint32_t first = 0;
    for (uint32_t g = 0; g < 7; ++g) {
        const uint32_t quads = source.group_quad_counts[g];
        if (quads) {
            layout.GroupBits[g] = 1u << layout.Ranges.size();
            layout.Ranges.push_back({first, quads * 6, 0});
        }
        first += quads * 6;
    }
    const bool needsWireframeGroups = layer != RN_MATERIAL_TRANSLUCENT && layout.Ranges.size() > 1;
    const bool hasWireframeFlag = (source.flags & RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS) != 0;
    if (needsWireframeGroups) {
        const uint64_t expectedBytes = (totalQuads * 3 + 7) / 8;
        if (!hasWireframeFlag || !source.original_quad_groups || source.original_quad_groups_bytes != expectedBytes) {
            RADRAY_ERR_LOG("Preanalyzed multi-group terrain mesh missing valid wireframe groups");
            return false;
        }
        layout.OriginalQuadGroups.assign(source.original_quad_groups, source.original_quad_groups + expectedBytes);
    } else if (hasWireframeFlag || source.original_quad_groups || source.original_quad_groups_bytes) {
        RADRAY_ERR_LOG("Unexpected wireframe group payload on single-group or translucent terrain mesh");
        return false;
    }
    return true;
}

bool WriteQuadIndices(std::span<radray::byte> destination, uint32_t vertexCount) noexcept {
    if (vertexCount % 4 || destination.size() != uint64_t(vertexCount / 4) * 24) return false;
    for (uint32_t q = 0; q < vertexCount / 4; ++q) {
        const uint32_t v = q * 4;
        const radray::array<uint32_t, 6> quad{v, v + 1, v + 2, v + 2, v + 3, v};
        std::memcpy(destination.data() + size_t(q) * 24, quad.data(), 24);
    }
    return true;
}

bool WriteWireframeIndices(std::span<radray::byte> destination, const TerrainMeshLayout& layout, uint32_t mask) noexcept {
    if (layout.Ranges.size() > 7 || layout.VertexCount % 4 || layout.Layer == RN_MATERIAL_TRANSLUCENT ||
        (mask & ~((1u << layout.Ranges.size()) - 1))) {
        return false;
    }
    uint64_t count = 0;
    for (size_t n = 0; n < layout.Ranges.size(); ++n) {
        if (mask & (1u << n)) count += layout.Ranges[n].Count;
    }
    if (destination.size() != count * 4) return false;
    if (!count) return true;
    if (layout.Ranges.size() <= 1) return WriteQuadIndices(destination, layout.VertexCount);
    const size_t quads = layout.VertexCount / 4;
    if (layout.OriginalQuadGroups.size() != (quads * 3 + 7) / 8) return false;
    radray::array<uint32_t, 7> positions{};
    for (uint32_t g = 0; g < 7; ++g) {
        if (layout.GroupBits[g]) {
            const auto bit = layout.GroupBits[g];
            const auto range = std::countr_zero(bit);
            if ((bit & (bit - 1)) || size_t(range) >= layout.Ranges.size() || layout.Ranges[range].First % 6) return false;
            positions[g] = layout.Ranges[range].First / 6;
        }
    }
    size_t output = 0;
    for (size_t q = 0; q < quads; ++q) {
        const size_t bit = q * 3;
        const size_t byte = bit / 8;
        const auto shift = uint32_t(bit % 8);
        uint32_t packed = layout.OriginalQuadGroups[byte];
        if (shift > 5) packed |= uint32_t(layout.OriginalQuadGroups[byte + 1]) << 8;
        const auto group = (packed >> shift) & 7u;
        if (group >= 7 || !layout.GroupBits[group]) return false;
        if (positions[group] >= quads) return false;
        const auto v = positions[group]++ * 4;
        if (!(layout.GroupBits[group] & mask)) continue;
        if (output + 24 > destination.size()) return false;
        const radray::array<uint32_t, 6> quad{v, v + 1, v + 2, v + 2, v + 3, v};
        std::memcpy(destination.data() + output, quad.data(), 24);
        output += 24;
    }
    return output == destination.size();
}

bool WriteLineIndices(std::span<radray::byte> destination, uint32_t vertexCount) noexcept {
    if (vertexCount % 4 || destination.size() != uint64_t(vertexCount / 4) * 24) return false;
    for (uint32_t q = 0; q < vertexCount / 4; ++q) {
        const uint32_t v = q * 4;
        const radray::array<uint32_t, 6> line{v, v + 1, v + 2, v + 3, v + 2, v + 1};
        std::memcpy(destination.data() + size_t(q) * 24, line.data(), 24);
    }
    return true;
}

TerrainEye ComputeTerrainEye(const rn_terrain_view_desc& frame) noexcept {
    const Eigen::Matrix4f matrix = Eigen::Map<const Eigen::Matrix4f>(frame.projection) * Eigen::Map<const Eigen::Matrix4f>(frame.view);
    if (!matrix.allFinite()) return {};
    const Eigen::FullPivLU<Eigen::Matrix4f> inverse(matrix);
    if (!inverse.isInvertible()) return {};
    const Eigen::Vector4f point = inverse.solve(Eigen::Vector4f(0, 0, 1, 0));
    if (!point.allFinite() || std::abs(point.w()) <= 1e-8f) return {};
    const auto eye = (point / point.w()).eval();
    return {{{eye.x(), eye.y(), eye.z()}}, true};
}

uint32_t TerrainVisibleMask(
    const TerrainMeshLayout& layout,
    const TerrainEye& eye,
    const rn_terrain_view_desc& frame,
    std::span<const rn_instance> instances) noexcept {
    const uint32_t full = (1u << layout.Ranges.size()) - 1;
    if (layout.Layer == RN_MATERIAL_TRANSLUCENT || !layout.FiniteBounds || !eye.Valid) return full;
    uint32_t mask = layout.GroupBits[6];
    for (const auto& instance : instances) {
        radray::array<double, 3> position;
        for (uint32_t a = 0; a < 3; ++a) {
            position[a] = double(int64_t(frame.camera_block[a]) - int64_t(instance.origin[a])) + frame.camera_fraction[a] + eye.Offset[a];
            if (!std::isfinite(position[a])) return full;
            if (position[a] >= layout.Minimum[a] - 0.001) mask |= layout.GroupBits[a * 2];
            if (position[a] <= layout.Maximum[a] + 0.001) mask |= layout.GroupBits[a * 2 + 1];
        }
        if (mask == full) break;
    }
    return mask;
}
}  // namespace raynium
