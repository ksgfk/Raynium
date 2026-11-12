#pragma once
#include <radray/types.h>
#include <radray/shader/shader_artifact.h>
#include <string_view>
#include <span>
#include <utility>

// See doc/architecture/shader-pack.md for the persisted container and loading contract.
namespace raynium {
inline constexpr uint32_t ShaderPackMagic=0x50534e52; // RNSP, little endian.
inline constexpr uint32_t ShaderPackVersion=1;
inline constexpr uint32_t ShaderPackMaxSize=64u<<20;
struct ShaderPackHeader {
    uint32_t magic,version,totalSize,entryCount,directoryOffset,reserved;
};
struct ShaderPackRecord {
    char name[64];
    uint32_t target,offset,size,crc32;
    radray::shader::GpuArtifactHash identity;
};
static_assert(sizeof(ShaderPackHeader)==24 && sizeof(ShaderPackRecord)==96);
struct ShaderPackEntry {
    std::string_view name;
    radray::shader::ShaderTarget target;
    std::span<const radray::byte> data;
    radray::shader::GpuArtifactHash identity;
};
uint32_t ShaderPackCrc32(std::span<const radray::byte>) noexcept;
bool ReadShaderPack(std::span<const radray::byte>,radray::vector<ShaderPackEntry>&);
inline constexpr radray::array<std::string_view,8> TerrainPrograms{"terrain","terrain_cutout","terrain_translucent",
    "terrain_oit_bounds","terrain_oit_transmittance","terrain_oit_accumulate","oit_bounds_cull","oit_composite"};
bool ValidateTerrainShader(const radray::shader::ShaderArtifactView&,std::string_view program);
inline constexpr radray::array<std::string_view,3> GuiPrograms{"gui_color","gui_textured","gui_text"};
bool ValidateGuiShader(const radray::shader::ShaderArtifactView&,std::string_view);

class ShaderProgram final {
public:
    static bool Create(std::span<const radray::byte>,std::string_view,radray::shader::ShaderTarget,
        radray::unique_ptr<ShaderProgram>&);
    static bool Create(std::span<const ShaderPackEntry>,std::string_view,radray::shader::ShaderTarget,
        radray::unique_ptr<ShaderProgram>&);
    const radray::shader::ShaderArtifactView& Artifact() const { return _artifact; }
    std::string_view Entry(radray::shader::ShaderStage stage) const;
private:
    explicit ShaderProgram(radray::shader::ShaderArtifactView artifact):_artifact(std::move(artifact)) {}
    radray::shader::ShaderArtifactView _artifact;
};
}
