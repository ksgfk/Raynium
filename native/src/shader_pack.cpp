#include <raynium/shader_pack.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include <radray/logger.h>
#include <radray/types.h>

namespace raynium {
uint32_t ShaderPackCrc32(std::span<const radray::byte> data) noexcept {
    uint32_t crc = ~0u;
    for (auto byte : data) {
        crc ^= std::to_integer<uint8_t>(byte);
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool ReadShaderPack(std::span<const radray::byte> data, radray::vector<ShaderPackEntry>& output) {
    static_assert(std::endian::native == std::endian::little);

    if (data.size() < sizeof(ShaderPackHeader) || data.size() > kShaderPackMaxSize) {
        RADRAY_ERR_LOG("Shader pack size is invalid");
        return false;
    }
    ShaderPackHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.Magic != kShaderPackMagic || header.Version != kShaderPackVersion || header.TotalSize != data.size() ||
        header.DirectoryOffset != sizeof(header) || header.Reserved || !header.EntryCount || header.EntryCount > 4096) {
        RADRAY_ERR_LOG("Shader pack header or version is invalid");
        return false;
    }
    const size_t directoryEnd = sizeof(header) + size_t(header.EntryCount) * sizeof(ShaderPackRecord);
    if (directoryEnd > data.size()) {
        RADRAY_ERR_LOG("Shader pack directory is truncated");
        return false;
    }
    radray::vector<ShaderPackEntry> entries;
    radray::set<std::pair<std::string_view, uint32_t>> names;
    radray::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (uint32_t i = 0; i < header.EntryCount; i++) {
        ShaderPackRecord record;
        std::memcpy(&record, data.data() + sizeof(header) + i * sizeof(record), sizeof(record));
        const auto end = std::find(std::begin(record.Name), std::end(record.Name), '\0');
        if (end == std::begin(record.Name) || end == std::end(record.Name) ||
            !std::all_of(end, std::end(record.Name), [](char c) { return c == 0; }) ||
            !std::all_of(std::begin(record.Name), end, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'; })) {
            RADRAY_ERR_LOG("Shader pack program name is invalid");
            return false;
        }
        if (record.Target > uint32_t(radray::shader::ShaderTarget::SPIRV) || record.Offset < directoryEnd || record.Offset % 8 ||
            !record.Size || record.Offset > data.size() || record.Size > data.size() - record.Offset) {
            RADRAY_ERR_LOG("Shader pack artifact range or target is invalid");
            return false;
        }
        const auto* storedName = reinterpret_cast<const char*>(data.data() + sizeof(header) + i * sizeof(record));
        const std::string_view name{storedName, size_t(end - std::begin(record.Name))};
        if (!names.emplace(name, record.Target).second) {
            RADRAY_ERR_LOG("Shader pack contains a duplicate program={} target={}", name, record.Target);
            return false;
        }
        auto payload = data.subspan(record.Offset, record.Size);
        if (ShaderPackCrc32(payload) != record.Crc32) {
            RADRAY_ERR_LOG("Shader pack artifact checksum mismatch: program={} target={}", name, record.Target);
            return false;
        }
        ranges.emplace_back(record.Offset, record.Offset + record.Size);
        entries.push_back({name, radray::shader::ShaderTarget(record.Target), payload, record.Identity});
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); i++) {
        if (ranges[i].first < ranges[i - 1].second) {
            RADRAY_ERR_LOG("Shader pack artifact ranges overlap");
            return false;
        }
    }
    output = std::move(entries);
    return true;
}

ShaderProgram::ShaderProgram(radray::shader::ShaderArtifactView artifact)
    : _artifact(std::move(artifact)) {}

const radray::shader::ShaderArtifactView& ShaderProgram::Artifact() const noexcept {
    return _artifact;
}

bool ShaderProgram::Create(
    std::span<const radray::byte> data,
    std::string_view name,
    radray::shader::ShaderTarget target,
    radray::unique_ptr<ShaderProgram>& output) {
    if (output) RADRAY_ABORT("Shader program output occupied");
    radray::vector<ShaderPackEntry> entries;
    if (!ReadShaderPack(data, entries)) return false;
    return Create(entries, name, target, output);
}

bool ShaderProgram::Create(
    std::span<const ShaderPackEntry> entries,
    std::string_view name,
    radray::shader::ShaderTarget target,
    radray::unique_ptr<ShaderProgram>& output) {
    if (output) RADRAY_ABORT("Shader program output occupied");
    auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& value) { return value.Name == name && value.Target == target; });
    if (entry == entries.end()) {
        RADRAY_ERR_LOG("Shader pack is missing program={} target={}", name, uint32_t(target));
        return false;
    }
    auto decoded = radray::shader::DecodeShaderArtifact(
        entry->Data,
        {.Target = target, .ExpectedGpuArtifact = entry->Identity, .ExpectedToolchainIdentity = radray::shader::kShaderToolchainIdentity});
    if (!decoded.Succeeded()) {
        RADRAY_ERR_LOG("Shader program={} target={} decode failed: {}", name, uint32_t(target), decoded.Diagnostics);
        return false;
    }
    output = radray::unique_ptr<ShaderProgram>(new ShaderProgram(std::move(*decoded.Value)));
    return true;
}

std::string_view ShaderProgram::Entry(radray::shader::ShaderStage stage) const {
    for (const auto& entry : _artifact.Entries()) {
        if (entry.Stage == uint8_t(stage)) return *_artifact.GetName(entry.Name);
    }
    return {};
}

bool ValidateOutputShader(const radray::shader::ShaderArtifactView& artifact) {
    const auto texture = artifact.FindBinding("SceneColor");
    const auto parameters = artifact.FindBinding("OutputParameters");
    if (artifact.Entries().size() != 2 ||
        !artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex) ||
        !artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel) ||
        !artifact.VertexInputs().empty() || !artifact.RootConstants().empty() || !artifact.Samplers().empty() ||
        !artifact.SerializedRootSignature().empty() || artifact.Bindings().size() != 2 || !texture || !parameters ||
        radray::shader::DecodeBindingKind(parameters->Record.Type) != radray::gpu::BindingKind::CBuffer || parameters->Record.Group ||
        parameters->Record.Binding != 1 || parameters->Record.StageMask != 2 || parameters->Record.Count != 1 ||
        parameters->Record.TypeIndex >= artifact.Types().size() || artifact.Types()[parameters->Record.TypeIndex].Size != 16 ||
        radray::shader::DecodeBindingKind(texture->Record.Type) != radray::gpu::BindingKind::Texture || texture->Record.Group || texture->Record.Binding ||
        texture->Record.Count != 1 || texture->Record.StageMask != 2 ||
        texture->Record.Placement != uint32_t(radray::shader::ShaderBindingPlacement::Table)) {
        RADRAY_ERR_LOG("Output shader interface is incompatible");
        return false;
    }
    return true;
}

bool ValidateTerrainShader(const radray::shader::ShaderArtifactView& artifact, std::string_view program) {
    if (artifact.Entries().size() != 2 ||
        !artifact.FindStageBytecode(radray::shader::ShaderStage::Vertex) ||
        !artifact.FindStageBytecode(radray::shader::ShaderStage::Pixel)) {
        RADRAY_ERR_LOG("{}: Terrain shader requires vertex and pixel stages", program);
        return false;
    }
    const bool vk = artifact.Envelope().Target == uint8_t(radray::shader::ShaderTarget::SPIRV);
    struct Binding {
        const char* Name;
        radray::gpu::BindingKind Kind;
        uint32_t Group;
        uint32_t Dx;
        uint32_t Vk;
        uint32_t Stages;
    };
    const bool alpha = program == "terrain_oit_bounds" || program == "terrain_oit_transmittance";
    const bool screen = program == "oit_bounds_cull" || program == "oit_composite";
    const bool composite = program == "oit_composite";
    radray::vector<Binding> bindings;
    if (!composite) bindings.push_back({"Frame", radray::gpu::BindingKind::CBuffer, 0, 0, 0, screen ? 2u : 3u});
    if (!screen) {
        bindings.push_back({"Atlas", radray::gpu::BindingKind::Texture, 1, 0, 1, 2});
        bindings.push_back({"AtlasSampler", radray::gpu::BindingKind::Sampler, 1, 0, 3, 2});
        if (!alpha) {
            bindings.push_back({"Lightmap", radray::gpu::BindingKind::Texture, 1, 1, 2, 1});
            bindings.push_back({"LightSampler", radray::gpu::BindingKind::Sampler, 1, 1, 4, 1});
        }
    }
    if (program == "terrain_oit_transmittance" || program == "terrain_oit_accumulate" || screen) {
        bindings.push_back({"DepthBounds", radray::gpu::BindingKind::Texture, 2, 0, 0, 2});
    }
    if (program == "terrain_oit_accumulate" || composite) {
        bindings.push_back({"Coeff0", radray::gpu::BindingKind::Texture, 2, 1, 1, 2});
        bindings.push_back({"Coeff1", radray::gpu::BindingKind::Texture, 2, 2, 2, 2});
    }
    if (composite) bindings.push_back({"Accumulation", radray::gpu::BindingKind::Texture, 2, 3, 3, 2});
    if (artifact.Bindings().size() != std::size(bindings) || !artifact.RootConstants().empty() || !artifact.Samplers().empty() ||
        !artifact.SerializedRootSignature().empty()) {
        RADRAY_ERR_LOG("{}: Terrain shader binding policy is incompatible", program);
        return false;
    }
    for (const auto& expected : bindings) {
        auto found = artifact.FindBinding(expected.Name);
        if (!found || radray::shader::DecodeBindingKind(found->Record.Type) != expected.Kind || found->Record.Group != expected.Group ||
            found->Record.Binding != (vk ? expected.Vk : expected.Dx) || found->Record.Count != 1 || found->Record.StageMask != expected.Stages ||
            found->Record.Placement != uint32_t(radray::shader::ShaderBindingPlacement::Table)) {
            RADRAY_ERR_LOG("{}: Terrain shader resource binding is incompatible", program);
            return false;
        }
    }
    auto frame = artifact.FindBinding("Frame");
    if (frame) {
        auto types = artifact.Types();
        const auto root = frame->Record.TypeIndex;
        if (root >= types.size() || types[root].Size != 224) {
            RADRAY_ERR_LOG("{}: Terrain Frame constant size is incompatible", program);
            return false;
        }
        struct Member {
            const char* Name;
            uint32_t Offset;
            uint32_t Size;
            uint32_t Scalar;
            uint32_t Rows;
        };
        const Member members[]{
            {"Projection", 0, 64, 1, 4},
            {"View", 64, 64, 1, 4},
            {"CameraBlock", 128, 16, 2, 1},
            {"CameraFraction", 144, 16, 1, 1},
            {"FogColor", 160, 16, 1, 1},
            {"FogDistances", 176, 16, 1, 1},
            {"AtlasSizeAndInverse", 192, 16, 1, 1},
            {"TerrainFade", 208, 16, 3, 1},
        };
        for (const auto& member : members) {
            auto found = std::find_if(types.begin(), types.end(), [&](const auto& type) {
                return type.ParentIndex == root && artifact.GetName(type.Name) == member.Name;
            });
            if (found == types.end() || found->Offset != member.Offset || found->Size != member.Size || found->ElementSize != member.Size ||
                found->Stride != 0 || found->MatrixStride != (member.Rows == 4 ? 16u : 0u) ||
                found->ScalarKind != member.Scalar || found->RowCount != member.Rows || found->ColumnCount != 4 || found->ElementCount != 1 ||
                found->Flags || found->Kind != uint32_t(member.Rows == 4 ? radray::shader::ShaderTypeKind::Matrix : radray::shader::ShaderTypeKind::Vector)) {
                RADRAY_ERR_LOG("{}: Terrain Frame constant member layout is incompatible", program);
                return false;
            }
        }
    }
    struct Input {
        const char* Semantic;
        uint32_t Index;
        uint32_t Location;
        uint32_t Type;
        uint32_t Count;
    };
    radray::vector<Input> inputs;
    if (!screen) {
        inputs = {{"POSITION", 0, 0, 1, 3}, {"COLOR", 0, 1, 1, 4}, {"TEXCOORD", 0, 2, 1, 2}};
        if (!alpha) inputs.push_back({"TEXCOORD", 1, 3, 3, 2});
        inputs.push_back({"TEXCOORD", 2, alpha ? 3u : 4u, 2, 3});
        if (!alpha) inputs.push_back({"TEXCOORD", 3, 5, 3, 1});
    }
    if (artifact.VertexInputs().size() != std::size(inputs)) {
        RADRAY_ERR_LOG("{}: Terrain vertex input count is incompatible", program);
        return false;
    }
    for (const auto& input : inputs) {
        auto found = std::find_if(artifact.VertexInputs().begin(), artifact.VertexInputs().end(), [&](const auto& value) {
            return artifact.GetName(value.Semantic) == input.Semantic && value.SemanticIndex == input.Index;
        });
        if (found == artifact.VertexInputs().end() || (vk && found->Location != input.Location) ||
            found->ComponentType != input.Type || found->ComponentCount != input.Count) {
            RADRAY_ERR_LOG("{}: Terrain vertex input layout is incompatible", program);
            return false;
        }
    }
    return true;
}
}  // namespace raynium
