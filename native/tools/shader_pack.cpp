#include <raynium/shader_pack.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <radray/logger.h>
#include <radray/shader_compiler/client.h>
#include <radray/file.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <span>
#include <utility>

namespace {
using namespace radray::shader;
struct ToolFailure {};
void Require(bool condition,std::string_view message,std::source_location location=std::source_location::current()) {
    if(!condition) {
        radray::LogFormatLoc(location,radray::LogLevel::Err,"{}",message);
        throw ToolFailure{};
    }
}
bool Verify(std::span<const radray::byte> data) {
    radray::vector<raynium::ShaderPackEntry> entries;
    if(!raynium::ReadShaderPack(data,entries)) return false;
    bool required[8][2]{};
    bool gui[3][2]{};
    for(const auto& entry:entries) {
        auto decoded=DecodeShaderArtifact(entry.data,{.Target=entry.target,.ExpectedGpuArtifact=entry.identity,.ExpectedToolchainIdentity=kShaderToolchainIdentity});
        if(!decoded.Succeeded()) { RADRAY_ERR_LOG("Shader program={} target={} decode failed: {}",entry.name,uint32_t(entry.target),decoded.Diagnostics); return false; }
        for(size_t i=0;i<raynium::TerrainPrograms.size();++i) if(entry.name==raynium::TerrainPrograms[i]) {
            if(!raynium::ValidateTerrainShader(*decoded.Value,entry.name)) return false;
            required[i][uint8_t(entry.target)]=true;
        }
        for(size_t i=0;i<raynium::GuiPrograms.size();++i) if(entry.name==raynium::GuiPrograms[i]) {
            if(!raynium::ValidateGuiShader(*decoded.Value,entry.name)) return false;
            gui[i][uint8_t(entry.target)]=true;
        }
    }
    for(size_t i=0;i<raynium::TerrainPrograms.size();++i)
        Require(required[i][0] && required[i][1],"Shader pack requires every terrain program for DXIL and SPIR-V");
    for(size_t i=0;i<raynium::GuiPrograms.size();++i)
        Require(gui[i][0] && gui[i][1],"Shader pack requires every GUI program for DXIL and SPIR-V");
    return true;
}
struct Program { radray::string name,source; };
struct Compiled { radray::string name; ShaderTarget target; radray::vector<radray::byte> data; GpuArtifactHash identity; };
}
int main(int argc,char** argv) {
    try {
        std::filesystem::path root,output,verify;
        radray::vector<std::filesystem::path> includes;
        radray::vector<Program> programs;
        for(int i=1;i<argc;i++) {
            std::string_view key=argv[i];
            Require(i+1<argc,"Every option requires a value");
            radray::string value=argv[++i];
            if(key=="--root") root=value;
            else if(key=="--output") output=value;
            else if(key=="--verify") verify=value;
            else if(key=="--include") includes.emplace_back(value);
            else if(key=="--program") {
                auto separator=value.find('=');
                Require(separator!=radray::string::npos,"Program must be name=logical/source.hlsl");
                programs.push_back({value.substr(0,separator),value.substr(separator+1)});
            } else { RADRAY_ERR_LOG("Unknown option: {}",key); return 1; }
        }
        if(!verify.empty()) {
            auto bytes=radray::ReadBinaryFile(verify);
            Require(bytes.has_value(),"Cannot read shader pack");
            if(!Verify(*bytes)) return 1; std::puts("Shader pack artifacts and terrain interfaces OK"); return 0;
        }
        Require(!root.empty() && !output.empty() && !programs.empty(),
            "usage: raynium_shader_pack --root <dir> --include <dir> --program <name=source.hlsl>... --output <file.rnsp> | --verify <file.rnsp>");
        includes.insert(includes.begin(),root);
        std::sort(programs.begin(),programs.end(),[](const auto& a,const auto& b){return a.name<b.name;});
        radray::shader_compiler::Client compiler;
        Require(compiler.IsAvailable(),"Pinned RadRay DXC compiler is unavailable; restore the SDK and add its bin directory to PATH");
        radray::vector<Compiled> compiled;
        for(const auto& program:programs) {
            Require(program.name.size()<64 && IsLogicalSourceName(program.source),"Invalid program name or logical source path");
            auto source=radray::ReadBinaryFile(root/program.source);
            Require(source.has_value(),"Cannot read shader source");
            auto contract=compiler.DiscoverSourceContract(program.source,*source,ShaderTarget::DXIL,includes);
            Require(contract.Succeeded(),contract.Diagnostics);
            Require(contract.Value->KeywordGroups.empty(),"Shader pack v1 does not select keyword variants");
            auto result=compiler.CompileVariant({.SourceName=radray::string{program.source},.RootSource=*source,.Targets=ShaderTargetMask::All,
                .ExpectedContract=contract.Value->Hash},includes);
            Require(result.Succeeded(),result.Diagnostics);
            if(!result.Diagnostics.empty()) std::fputs(result.Diagnostics.c_str(),stderr);
            for(auto& lane:*result.Value) {
                Require(lane.Metadata.size()>=sizeof(WireMetadataEnvelope),"Compiler metadata is truncated");
                WireMetadataEnvelope envelope; std::memcpy(&envelope,lane.Metadata.data(),sizeof(envelope));
                compiled.push_back({program.name,lane.Target,std::move(lane.Metadata),envelope.GpuArtifact});
            }
        }
        std::sort(compiled.begin(),compiled.end(),[](const auto& a,const auto& b){return std::pair{a.name,a.target}<std::pair{b.name,b.target};});
        Require(compiled.size()<=4096,"Too many shader programs");
        radray::vector<radray::byte> bytes(sizeof(raynium::ShaderPackHeader)+compiled.size()*sizeof(raynium::ShaderPackRecord));
        for(size_t i=0;i<compiled.size();i++) {
            const auto& item=compiled[i];
            bytes.resize((bytes.size()+7)&~size_t(7));
            Require(item.data.size()<=raynium::ShaderPackMaxSize && bytes.size()<=raynium::ShaderPackMaxSize-item.data.size(),"Shader pack is too large");
            raynium::ShaderPackRecord record{};
            std::memcpy(record.name,item.name.data(),item.name.size());
            record.target=uint32_t(item.target); record.offset=uint32_t(bytes.size()); record.size=uint32_t(item.data.size());
            record.crc32=raynium::ShaderPackCrc32(item.data); record.identity=item.identity;
            std::memcpy(bytes.data()+sizeof(raynium::ShaderPackHeader)+i*sizeof(record),&record,sizeof(record));
            bytes.insert(bytes.end(),item.data.begin(),item.data.end());
        }
        const raynium::ShaderPackHeader header{raynium::ShaderPackMagic,raynium::ShaderPackVersion,uint32_t(bytes.size()),uint32_t(compiled.size()),sizeof(raynium::ShaderPackHeader),0};
        std::memcpy(bytes.data(),&header,sizeof(header)); if(!Verify(bytes)) return 1;
        Require(radray::WriteBinaryFile(output,bytes),"Cannot write shader pack");
        std::printf("Wrote shader pack: %zu programs, %zu bytes\n",programs.size(),bytes.size()); return 0;
    } catch(const ToolFailure&) {
        return 1;
    } catch(...) {
        raynium::AbortUnexpectedException("raynium_shader_pack");
    }
}
