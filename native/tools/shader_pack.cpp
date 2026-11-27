#include <raynium/shader_pack.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/shader/client.h>
#include <radray/types.h>
#include <raynium/exception.h>

namespace {
struct ToolFailure {};

void Require(bool condition, std::string_view message, std::source_location location = std::source_location::current()) {
    if (!condition) {
        radray::LogFormatLoc(location, radray::LogLevel::Err, "{}", message);
        throw ToolFailure{};
    }
}

bool Verify(std::span<const radray::byte> data, std::string_view standalone) {
    radray::vector<raynium::ShaderPackEntry> entries;
    if (!raynium::ReadShaderPack(data, entries)) return false;
    bool required[raynium::kTerrainPrograms.size()][2]{};
    bool output[2]{};
    for (const auto& entry : entries) {
        auto decoded = radray::shader::DecodeShaderArtifact(
            entry.Data,
            {
                .Target = entry.Target,
                .ExpectedGpuArtifact = entry.Identity,
                .ExpectedToolchainIdentity = radray::shader::kShaderToolchainIdentity,
            });
        if (!decoded.Succeeded()) {
            RADRAY_ERR_LOG("Shader program={} target={} decode failed: {}", entry.Name, uint32_t(entry.Target), decoded.Diagnostics);
            return false;
        }
        if (entry.Name == "output") {
            if (!raynium::ValidateOutputShader(*decoded.Value)) return false;
            output[uint8_t(entry.Target)] = true;
        }
        for (size_t i = 0; i < raynium::kTerrainPrograms.size(); ++i) {
            if (entry.Name == raynium::kTerrainPrograms[i]) {
                if (!raynium::ValidateTerrainShader(*decoded.Value, entry.Name)) return false;
                required[i][uint8_t(entry.Target)] = true;
            }
        }
    }
    if (!standalone.empty()) {
        return entries.size() == 2 && entries[0].Name == standalone && entries[1].Name == standalone && entries[0].Target != entries[1].Target;
    }
    for (size_t i = 0; i < raynium::kTerrainPrograms.size(); ++i) {
        Require(required[i][0] && required[i][1], "Shader pack requires every terrain program for DXIL and SPIR-V");
    }
    Require(output[0] && output[1], "Shader pack requires the output program for DXIL and SPIR-V");
    return true;
}

struct Program {
    radray::string Name;
    radray::string Source;
};

struct Compiled {
    radray::string Name;
    radray::shader::ShaderTarget Target{};
    radray::vector<radray::byte> Data;
    radray::shader::GpuArtifactHash Identity{};
};
}  // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path root;
        std::filesystem::path output;
        std::filesystem::path verify;
        radray::string standalone;
        radray::vector<std::filesystem::path> includes;
        radray::vector<Program> programs;
        for (int i = 1; i < argc; i++) {
            const std::string_view key = argv[i];
            Require(i + 1 < argc, "Every option requires a value");
            const radray::string value = argv[++i];
            if (key == "--root")
                root = value;
            else if (key == "--output")
                output = value;
            else if (key == "--standalone")
                standalone = value;
            else if (key == "--verify")
                verify = value;
            else if (key == "--include")
                includes.emplace_back(value);
            else if (key == "--program-list") {
                std::ifstream manifest{std::filesystem::path(value)};
                Require(manifest.is_open(), "Cannot open program list");
                std::string line;
                while (std::getline(manifest, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    const auto split = line.find('=');
                    Require(split != std::string::npos, "Invalid program list entry");
                    programs.push_back({radray::string(line.data(), split), radray::string(line.data() + split + 1, line.size() - split - 1)});
                }
            } else if (key == "--program") {
                const auto separator = value.find('=');
                Require(separator != std::string::npos, "Program must be name=logical/source.hlsl");
                programs.push_back({value.substr(0, separator), value.substr(separator + 1)});
            } else {
                RADRAY_ERR_LOG("Unknown option: {}", key);
                return 1;
            }
        }
        if (!verify.empty()) {
            auto bytes = radray::ReadBinaryFile(verify);
            Require(bytes.has_value(), "Cannot read shader pack");
            if (!Verify(*bytes, standalone)) return 1;
            std::puts("Shader pack artifacts and terrain interfaces OK");
            return 0;
        }
        Require(
            !root.empty() && !output.empty() && !programs.empty(),
            "usage: raynium_shader_pack --root <dir> --include <dir> --program <name=source.hlsl>... --output <file.rnsp> | --verify <file.rnsp>");
        includes.insert(includes.begin(), root);
        std::sort(programs.begin(), programs.end(), [](const auto& a, const auto& b) { return a.Name < b.Name; });
        radray::shader::Client compiler;
        Require(compiler.IsAvailable(), "Pinned RadRay DXC compiler is unavailable; restore the SDK and add its bin directory to PATH");
        radray::vector<Compiled> compiled;
        for (const auto& program : programs) {
            Require(program.Name.size() < 64 && radray::shader::IsLogicalSourceName(program.Source), "Invalid program name or logical source path");
            auto source = radray::ReadBinaryFile(root / program.Source);
            Require(source.has_value(), "Cannot read shader source");
            auto contract = compiler.DiscoverSourceContract(program.Source, *source, radray::shader::ShaderTarget::DXIL, includes);
            Require(contract.Succeeded(), contract.Diagnostics);
            Require(contract.Value->KeywordGroups.empty(), "Shader pack v1 does not select keyword variants");
            auto result = compiler.CompileVariant(
                {
                    .SourceName = radray::string{program.Source},
                    .RootSource = *source,
                    .Targets = radray::shader::ShaderTargetMask::All,
                    .ExpectedContract = contract.Value->Hash,
                },
                includes);
            Require(result.Succeeded(), result.Diagnostics);
            if (!result.Diagnostics.empty()) std::fputs(result.Diagnostics.c_str(), stderr);
            for (auto& lane : *result.Value) {
                Require(lane.Metadata.size() >= sizeof(radray::shader::WireMetadataEnvelope), "Compiler metadata is truncated");
                radray::shader::WireMetadataEnvelope envelope;
                std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
                compiled.push_back({program.Name, lane.Target, std::move(lane.Metadata), envelope.GpuArtifact});
            }
        }
        std::sort(compiled.begin(), compiled.end(), [](const auto& a, const auto& b) { return std::pair{a.Name, a.Target} < std::pair{b.Name, b.Target}; });
        Require(compiled.size() <= 4096, "Too many shader programs");
        radray::vector<radray::byte> bytes(sizeof(raynium::ShaderPackHeader) + compiled.size() * sizeof(raynium::ShaderPackRecord));
        for (size_t i = 0; i < compiled.size(); i++) {
            const auto& item = compiled[i];
            bytes.resize((bytes.size() + 7) & ~size_t(7));
            Require(item.Data.size() <= raynium::kShaderPackMaxSize && bytes.size() <= raynium::kShaderPackMaxSize - item.Data.size(), "Shader pack is too large");
            raynium::ShaderPackRecord record{};
            std::memcpy(record.Name, item.Name.data(), item.Name.size());
            record.Target = uint32_t(item.Target);
            record.Offset = uint32_t(bytes.size());
            record.Size = uint32_t(item.Data.size());
            record.Crc32 = raynium::ShaderPackCrc32(item.Data);
            record.Identity = item.Identity;
            std::memcpy(bytes.data() + sizeof(raynium::ShaderPackHeader) + i * sizeof(record), &record, sizeof(record));
            bytes.insert(bytes.end(), item.Data.begin(), item.Data.end());
        }
        const raynium::ShaderPackHeader header{
            raynium::kShaderPackMagic,
            raynium::kShaderPackVersion,
            uint32_t(bytes.size()),
            uint32_t(compiled.size()),
            sizeof(raynium::ShaderPackHeader),
            0,
        };
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (!Verify(bytes, standalone)) return 1;
        Require(radray::WriteBinaryFile(output, bytes), "Cannot write shader pack");
        std::printf("Wrote shader pack: %zu programs, %zu bytes\n", programs.size(), bytes.size());
        return 0;
    } catch (const ToolFailure&) {
        return 1;
    } catch (...) {
        raynium::AbortUnexpectedException("raynium_shader_pack");
    }
}
