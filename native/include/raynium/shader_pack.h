#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <radray/shader/shader_artifact.h>
#include <radray/types.h>

// See doc/architecture/shader-pack.md for the persisted container and loading contract.
namespace raynium {
inline constexpr uint32_t kShaderPackMagic = 0x50534e52;  // RNSP, little endian.
inline constexpr uint32_t kShaderPackVersion = 1;
inline constexpr uint32_t kShaderPackMaxSize = 64u << 20;

struct ShaderPackHeader {
    uint32_t Magic{};
    uint32_t Version{};
    uint32_t TotalSize{};
    uint32_t EntryCount{};
    uint32_t DirectoryOffset{};
    uint32_t Reserved{};
};

struct ShaderPackRecord {
    char Name[64]{};
    uint32_t Target{};
    uint32_t Offset{};
    uint32_t Size{};
    uint32_t Crc32{};
    radray::shader::GpuArtifactHash Identity{};
};
static_assert(sizeof(ShaderPackHeader) == 24 && sizeof(ShaderPackRecord) == 96);

struct ShaderPackEntry {
    std::string_view Name;
    radray::shader::ShaderTarget Target{};
    std::span<const radray::byte> Data;
    radray::shader::GpuArtifactHash Identity{};
};

uint32_t ShaderPackCrc32(std::span<const radray::byte> data) noexcept;
bool ReadShaderPack(std::span<const radray::byte> data, radray::vector<ShaderPackEntry>& output);

inline constexpr radray::array<std::string_view, 6> kTerrainPrograms{
    "terrain",
    "terrain_cutout",
    "terrain_translucent",
    "terrain_oit_bounds",
    "terrain_oit_transmittance",
    "terrain_oit_accumulate",
};

bool ValidateTerrainShader(const radray::shader::ShaderArtifactView& artifact, std::string_view program);
bool ValidateOutputShader(const radray::shader::ShaderArtifactView& artifact);

class ShaderProgram final {
public:
    static bool Create(
        std::span<const radray::byte> data,
        std::string_view name,
        radray::shader::ShaderTarget target,
        radray::unique_ptr<ShaderProgram>& output);
    static bool Create(
        std::span<const ShaderPackEntry> entries,
        std::string_view name,
        radray::shader::ShaderTarget target,
        radray::unique_ptr<ShaderProgram>& output);
    const radray::shader::ShaderArtifactView& Artifact() const noexcept;
    std::string_view Entry(radray::shader::ShaderStage stage) const;

private:
    explicit ShaderProgram(radray::shader::ShaderArtifactView artifact);

    radray::shader::ShaderArtifactView _artifact;
};
}  // namespace raynium
