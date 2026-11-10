#include <raynium/shader_pack.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <set>

namespace raynium {
using namespace radray::shader;
uint32_t ShaderPackCrc32(std::span<const radray::byte> data) noexcept {
    uint32_t crc=~0u;
    for(auto byte:data) {
        crc^=std::to_integer<uint8_t>(byte);
        for(int i=0;i<8;i++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
bool ReadShaderPack(std::span<const radray::byte> data,std::vector<ShaderPackEntry>& output,radray::string& error) {
    static_assert(std::endian::native==std::endian::little);
    auto fail=[&](const char* message) { error=message; return false; };
    if(data.size()<sizeof(ShaderPackHeader) || data.size()>ShaderPackMaxSize) return fail("Shader pack size is invalid");
    ShaderPackHeader header; std::memcpy(&header,data.data(),sizeof(header));
    if(header.magic!=ShaderPackMagic || header.version!=ShaderPackVersion || header.totalSize!=data.size() ||
       header.directoryOffset!=sizeof(header) || header.reserved || !header.entryCount || header.entryCount>4096)
        return fail("Shader pack header or version is invalid");
    const size_t directoryEnd=sizeof(header)+size_t(header.entryCount)*sizeof(ShaderPackRecord);
    if(directoryEnd>data.size()) return fail("Shader pack directory is truncated");
    std::vector<ShaderPackEntry> entries;
    std::set<std::pair<std::string_view,uint32_t>> names;
    std::vector<std::pair<uint32_t,uint32_t>> ranges;
    for(uint32_t i=0;i<header.entryCount;i++) {
        ShaderPackRecord record; std::memcpy(&record,data.data()+sizeof(header)+i*sizeof(record),sizeof(record));
        const auto end=std::find(std::begin(record.name),std::end(record.name),'\0');
        if(end==std::begin(record.name) || end==std::end(record.name) ||
           !std::all_of(end,std::end(record.name),[](char c){return c==0;}) ||
           !std::all_of(std::begin(record.name),end,[](char c){return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.' || c=='/';}))
            return fail("Shader pack program name is invalid");
        if(record.target>uint32_t(ShaderTarget::SPIRV) || record.offset<directoryEnd || record.offset%8 ||
           !record.size || record.offset>data.size() || record.size>data.size()-record.offset)
            return fail("Shader pack artifact range or target is invalid");
        const auto* storedName=reinterpret_cast<const char*>(data.data()+sizeof(header)+i*sizeof(record));
        const std::string_view name{storedName,size_t(end-std::begin(record.name))};
        if(!names.emplace(name,record.target).second) return fail("Shader pack contains a duplicate program target");
        auto payload=data.subspan(record.offset,record.size);
        if(ShaderPackCrc32(payload)!=record.crc32) return fail("Shader pack artifact checksum mismatch");
        ranges.emplace_back(record.offset,record.offset+record.size);
        entries.push_back({name,ShaderTarget(record.target),payload,record.identity});
    }
    std::sort(ranges.begin(),ranges.end());
    for(size_t i=1;i<ranges.size();i++) if(ranges[i].first<ranges[i-1].second) return fail("Shader pack artifact ranges overlap");
    output=std::move(entries); return true;
}
bool ShaderProgram::Create(std::span<const radray::byte> data,std::string_view name,ShaderTarget target,
    std::unique_ptr<ShaderProgram>& output,radray::string& error) {
    if(output) { error="Shader program output occupied"; return false; }
    std::vector<ShaderPackEntry> entries;
    if(!ReadShaderPack(data,entries,error)) return false;
    auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& value){return value.name==name && value.target==target;});
    if(entry==entries.end()) { error="Shader pack is missing the requested program/backend"; return false; }
    auto decoded=DecodeShaderArtifact(entry->data,{.Target=target,.ExpectedGpuArtifact=entry->identity,.ExpectedToolchainIdentity=kShaderToolchainIdentity});
    if(!decoded.Succeeded()) { error=decoded.Diagnostics; return false; }
    output=std::unique_ptr<ShaderProgram>(new ShaderProgram(std::move(*decoded.Value))); return true;
}
std::string_view ShaderProgram::Entry(ShaderStage stage) const {
    for(const auto& entry:_artifact.Entries()) if(entry.Stage==uint8_t(stage)) return *_artifact.GetName(entry.Name);
    return {};
}
bool ValidateTerrainShader(const ShaderArtifactView& artifact,radray::string& error) {
    auto fail=[&](const char* message) { error=message; return false; };
    if(artifact.Entries().size()!=2 || !artifact.FindStageBytecode(ShaderStage::Vertex) || !artifact.FindStageBytecode(ShaderStage::Pixel))
        return fail("Terrain shader requires vertex and pixel stages");
    const bool vk=artifact.Envelope().Target==uint8_t(ShaderTarget::SPIRV);
    struct Binding { const char* name; ShaderBindingKind kind; uint32_t group,dx,vk,stages; };
    const Binding bindings[]{
        {"Frame",ShaderBindingKind::CBuffer,0,0,0,3}, {"Atlas",ShaderBindingKind::Texture,1,0,1,2},
        {"Lightmap",ShaderBindingKind::Texture,1,1,2,1}, {"AtlasSampler",ShaderBindingKind::Sampler,1,0,3,2},
        {"LightSampler",ShaderBindingKind::Sampler,1,1,4,1}};
    if(artifact.Bindings().size()!=std::size(bindings) || !artifact.RootConstants().empty() || !artifact.Samplers().empty() ||
       !artifact.SerializedRootSignature().empty()) return fail("Terrain shader binding policy is incompatible");
    for(const auto& expected:bindings) {
        auto found=artifact.FindBinding(expected.name);
        if(!found || found->Record.Type!=uint32_t(expected.kind) || found->Record.Group!=expected.group ||
           found->Record.Binding!=(vk?expected.vk:expected.dx) || found->Record.Count!=1 || found->Record.StageMask!=expected.stages ||
           found->Record.Placement!=uint32_t(ShaderBindingPlacement::Table)) return fail("Terrain shader resource binding is incompatible");
    }
    auto frame=artifact.FindBinding("Frame");
    auto types=artifact.Types(); const auto root=frame->Record.TypeIndex;
    if(root>=types.size() || types[root].Size!=192) return fail("Terrain Frame constant size is incompatible");
    struct Member { const char* name; uint32_t offset,size,scalar,rows; };
    const Member members[]{{"Projection",0,64,1,4},{"View",64,64,1,4},{"CameraBlock",128,16,2,1},
        {"CameraFraction",144,16,1,1},{"FogColor",160,16,1,1},{"FogDistances",176,16,1,1}};
    for(const auto& member:members) {
        auto found=std::find_if(types.begin(),types.end(),[&](const auto& type){return type.ParentIndex==root && artifact.GetName(type.Name)==member.name;});
        if(found==types.end() || found->Offset!=member.offset || found->Size!=member.size || found->Stride!=member.size ||
           found->ScalarKind!=member.scalar || found->RowCount!=member.rows || found->ColumnCount!=4 || found->ElementCount!=1 ||
           found->Flags || found->Kind!=uint32_t(member.rows==4?ShaderTypeKind::Matrix:ShaderTypeKind::Vector))
            return fail("Terrain Frame constant member layout is incompatible");
    }
    struct Input { const char* semantic; uint32_t index,location,type,count; };
    const Input inputs[]{{"POSITION",0,0,1,3},{"COLOR",0,1,1,4},{"TEXCOORD",0,2,1,2},{"TEXCOORD",1,3,3,2},{"TEXCOORD",2,4,2,3},{"TEXCOORD",3,5,1,1}};
    if(artifact.VertexInputs().size()!=std::size(inputs)) return fail("Terrain vertex input count is incompatible");
    for(const auto& input:inputs) {
        auto found=std::find_if(artifact.VertexInputs().begin(),artifact.VertexInputs().end(),[&](const auto& value){
            return artifact.GetName(value.Semantic)==input.semantic && value.SemanticIndex==input.index;
        });
        if(found==artifact.VertexInputs().end() || (vk && found->Location!=input.location) ||
           found->ComponentType!=input.type || found->ComponentCount!=input.count) return fail("Terrain vertex input layout is incompatible");
    }
    return true;
}
}
