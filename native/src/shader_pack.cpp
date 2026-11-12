#include <raynium/shader_pack.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <string_view>
#include <span>
#include <utility>

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
bool ReadShaderPack(std::span<const radray::byte> data,radray::vector<ShaderPackEntry>& output) {
    static_assert(std::endian::native==std::endian::little);

    if(data.size()<sizeof(ShaderPackHeader) || data.size()>ShaderPackMaxSize) { RADRAY_ERR_LOG("Shader pack size is invalid"); return false; }
    ShaderPackHeader header; std::memcpy(&header,data.data(),sizeof(header));
    if(header.magic!=ShaderPackMagic || header.version!=ShaderPackVersion || header.totalSize!=data.size() ||
       header.directoryOffset!=sizeof(header) || header.reserved || !header.entryCount || header.entryCount>4096)
        { RADRAY_ERR_LOG("Shader pack header or version is invalid"); return false; }
    const size_t directoryEnd=sizeof(header)+size_t(header.entryCount)*sizeof(ShaderPackRecord);
    if(directoryEnd>data.size()) { RADRAY_ERR_LOG("Shader pack directory is truncated"); return false; }
    radray::vector<ShaderPackEntry> entries;
    radray::set<std::pair<std::string_view,uint32_t>> names;
    radray::vector<std::pair<uint32_t,uint32_t>> ranges;
    for(uint32_t i=0;i<header.entryCount;i++) {
        ShaderPackRecord record; std::memcpy(&record,data.data()+sizeof(header)+i*sizeof(record),sizeof(record));
        const auto end=std::find(std::begin(record.name),std::end(record.name),'\0');
        if(end==std::begin(record.name) || end==std::end(record.name) ||
           !std::all_of(end,std::end(record.name),[](char c){return c==0;}) ||
           !std::all_of(std::begin(record.name),end,[](char c){return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.' || c=='/';}))
            { RADRAY_ERR_LOG("Shader pack program name is invalid"); return false; }
        if(record.target>uint32_t(ShaderTarget::SPIRV) || record.offset<directoryEnd || record.offset%8 ||
           !record.size || record.offset>data.size() || record.size>data.size()-record.offset)
            { RADRAY_ERR_LOG("Shader pack artifact range or target is invalid"); return false; }
        const auto* storedName=reinterpret_cast<const char*>(data.data()+sizeof(header)+i*sizeof(record));
        const std::string_view name{storedName,size_t(end-std::begin(record.name))};
        if(!names.emplace(name,record.target).second) { RADRAY_ERR_LOG("Shader pack contains a duplicate program={} target={}",name,record.target); return false; }
        auto payload=data.subspan(record.offset,record.size);
        if(ShaderPackCrc32(payload)!=record.crc32) { RADRAY_ERR_LOG("Shader pack artifact checksum mismatch: program={} target={}",name,record.target); return false; }
        ranges.emplace_back(record.offset,record.offset+record.size);
        entries.push_back({name,ShaderTarget(record.target),payload,record.identity});
    }
    std::sort(ranges.begin(),ranges.end());
    for(size_t i=1;i<ranges.size();i++) if(ranges[i].first<ranges[i-1].second) { RADRAY_ERR_LOG("Shader pack artifact ranges overlap"); return false; }
    output=std::move(entries); return true;
}
bool ShaderProgram::Create(std::span<const radray::byte> data,std::string_view name,ShaderTarget target,
    radray::unique_ptr<ShaderProgram>& output) {
    if(output) RADRAY_ABORT("Shader program output occupied");
    radray::vector<ShaderPackEntry> entries;
    if(!ReadShaderPack(data,entries)) return false;
    return Create(entries,name,target,output);
}
bool ShaderProgram::Create(std::span<const ShaderPackEntry> entries,std::string_view name,ShaderTarget target,
    radray::unique_ptr<ShaderProgram>& output) {
    if(output) RADRAY_ABORT("Shader program output occupied");
    auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& value){return value.name==name && value.target==target;});
    if(entry==entries.end()) { RADRAY_ERR_LOG("Shader pack is missing program={} target={}",name,uint32_t(target)); return false; }
    auto decoded=DecodeShaderArtifact(entry->data,{.Target=target,.ExpectedGpuArtifact=entry->identity,.ExpectedToolchainIdentity=kShaderToolchainIdentity});
    if(!decoded.Succeeded()) { RADRAY_ERR_LOG("Shader program={} target={} decode failed: {}",name,uint32_t(target),decoded.Diagnostics); return false; }
    output=radray::unique_ptr<ShaderProgram>(new ShaderProgram(std::move(*decoded.Value))); return true;
}
std::string_view ShaderProgram::Entry(ShaderStage stage) const {
    for(const auto& entry:_artifact.Entries()) if(entry.Stage==uint8_t(stage)) return *_artifact.GetName(entry.Name);
    return {};
}
bool ValidateGuiShader(const ShaderArtifactView& artifact,std::string_view program) {

    const bool textured=program!="gui_color",text=program=="gui_text",vk=artifact.Envelope().Target==uint8_t(ShaderTarget::SPIRV);
    if(std::find(GuiPrograms.begin(),GuiPrograms.end(),program)==GuiPrograms.end() || artifact.Entries().size()!=2 ||
       !artifact.FindStageBytecode(ShaderStage::Vertex) || !artifact.FindStageBytecode(ShaderStage::Pixel)) { RADRAY_ERR_LOG("{}: GUI shader requires known vertex/pixel program",program); return false; }
    if(!artifact.RootConstants().empty() || !artifact.Samplers().empty() || !artifact.SerializedRootSignature().empty() ||
       artifact.Bindings().size()!=(textured?3u:1u)) { RADRAY_ERR_LOG("{}: GUI shader binding policy is incompatible",program); return false; }
    const auto binding=[&](const char* name,ShaderBindingKind kind,uint32_t group,uint32_t dx,uint32_t slot,uint32_t stages) {
        auto found=artifact.FindBinding(name);
        return found && found->Record.Type==uint32_t(kind) && found->Record.Group==group && found->Record.Binding==(vk?slot:dx) &&
            found->Record.Count==1 && found->Record.StageMask==stages && found->Record.Placement==uint32_t(ShaderBindingPlacement::Table);
    };
    if(!binding("GuiFrame",ShaderBindingKind::CBuffer,0,0,0,text?3u:1u) ||
       (textured && (!binding("GuiTexture",ShaderBindingKind::Texture,1,0,0,2) || !binding("GuiSampler",ShaderBindingKind::Sampler,1,0,1,2))))
        { RADRAY_ERR_LOG("{}: GUI shader resource binding is incompatible",program); return false; }
    auto frame=artifact.FindBinding("GuiFrame"); auto types=artifact.Types();
    if(frame->Record.TypeIndex>=types.size() || types[frame->Record.TypeIndex].Size!=16) { RADRAY_ERR_LOG("{}: GUI constant size is incompatible",program); return false; }
    const auto member=std::find_if(types.begin(),types.end(),[&](const auto& type) {
        return type.ParentIndex==frame->Record.TypeIndex && artifact.GetName(type.Name)=="GuiViewport";
    });
    if(member==types.end() || member->Offset!=0 || member->Size!=16 || member->ScalarKind!=1 || member->RowCount!=1 || member->ColumnCount!=4)
        { RADRAY_ERR_LOG("{}: GUI constant layout is incompatible",program); return false; }
    if(artifact.VertexInputs().size()!=(textured?3u:2u)) { RADRAY_ERR_LOG("{}: GUI vertex input count is incompatible",program); return false; }
    struct Input { const char* name; uint32_t location,count; };
    const Input inputs[]{{"POSITION",0,3},{"COLOR",textured?2u:1u,4},{"TEXCOORD",1,2}};
    for(size_t i=0;i<(textured?3u:2u);++i) {
        const auto& expected=inputs[i];
        auto input=std::find_if(artifact.VertexInputs().begin(),artifact.VertexInputs().end(),[&](const auto& value) {
            return artifact.GetName(value.Semantic)==expected.name && value.SemanticIndex==0;
        });
        if(input==artifact.VertexInputs().end() || (vk && input->Location!=expected.location) || input->ComponentType!=1 || input->ComponentCount!=expected.count)
            { RADRAY_ERR_LOG("{}: GUI vertex input layout is incompatible",program); return false; }
    }
    return true;
}
bool ValidateTerrainShader(const ShaderArtifactView& artifact,std::string_view program) {

    if(artifact.Entries().size()!=2 || !artifact.FindStageBytecode(ShaderStage::Vertex) || !artifact.FindStageBytecode(ShaderStage::Pixel))
        { RADRAY_ERR_LOG("{}: Terrain shader requires vertex and pixel stages",program); return false; }
    const bool vk=artifact.Envelope().Target==uint8_t(ShaderTarget::SPIRV);
    struct Binding { const char* name; ShaderBindingKind kind; uint32_t group,dx,vk,stages; };
    const bool alpha=program=="terrain_oit_bounds" || program=="terrain_oit_transmittance";
    const bool screen=program=="oit_bounds_cull" || program=="oit_composite";
    const bool composite=program=="oit_composite";
    radray::vector<Binding> bindings;
    if(!composite) bindings.push_back({"Frame",ShaderBindingKind::CBuffer,0,0,0,screen?2u:3u});
    if(!screen) {
        bindings.push_back({"Atlas",ShaderBindingKind::Texture,1,0,1,2});
        bindings.push_back({"AtlasSampler",ShaderBindingKind::Sampler,1,0,3,2});
        if(!alpha) {
            bindings.push_back({"Lightmap",ShaderBindingKind::Texture,1,1,2,1});
            bindings.push_back({"LightSampler",ShaderBindingKind::Sampler,1,1,4,1});
        }
    }
    if(program=="terrain_oit_transmittance" || program=="terrain_oit_accumulate" || screen)
        bindings.push_back({"DepthBounds",ShaderBindingKind::Texture,2,0,0,2});
    if(program=="terrain_oit_accumulate" || composite) {
        bindings.push_back({"Coeff0",ShaderBindingKind::Texture,2,1,1,2});
        bindings.push_back({"Coeff1",ShaderBindingKind::Texture,2,2,2,2});
    }
    if(composite) bindings.push_back({"Accumulation",ShaderBindingKind::Texture,2,3,3,2});
    if(artifact.Bindings().size()!=std::size(bindings) || !artifact.RootConstants().empty() || !artifact.Samplers().empty() ||
       !artifact.SerializedRootSignature().empty()) { RADRAY_ERR_LOG("{}: Terrain shader binding policy is incompatible",program); return false; }
    for(const auto& expected:bindings) {
        auto found=artifact.FindBinding(expected.name);
        if(!found || found->Record.Type!=uint32_t(expected.kind) || found->Record.Group!=expected.group ||
           found->Record.Binding!=(vk?expected.vk:expected.dx) || found->Record.Count!=1 || found->Record.StageMask!=expected.stages ||
           found->Record.Placement!=uint32_t(ShaderBindingPlacement::Table)) { RADRAY_ERR_LOG("{}: Terrain shader resource binding is incompatible",program); return false; }
    }
    auto frame=artifact.FindBinding("Frame");
    if(frame) {
        auto types=artifact.Types(); const auto root=frame->Record.TypeIndex;
        if(root>=types.size() || types[root].Size!=192) { RADRAY_ERR_LOG("{}: Terrain Frame constant size is incompatible",program); return false; }
        struct Member { const char* name; uint32_t offset,size,scalar,rows; };
        const Member members[]{{"Projection",0,64,1,4},{"View",64,64,1,4},{"CameraBlock",128,16,2,1},
            {"CameraFraction",144,16,1,1},{"FogColor",160,16,1,1},{"FogDistances",176,16,1,1}};
        for(const auto& member:members) {
            auto found=std::find_if(types.begin(),types.end(),[&](const auto& type){return type.ParentIndex==root && artifact.GetName(type.Name)==member.name;});
            if(found==types.end() || found->Offset!=member.offset || found->Size!=member.size || found->Stride!=member.size ||
               found->ScalarKind!=member.scalar || found->RowCount!=member.rows || found->ColumnCount!=4 || found->ElementCount!=1 ||
               found->Flags || found->Kind!=uint32_t(member.rows==4?ShaderTypeKind::Matrix:ShaderTypeKind::Vector))
                { RADRAY_ERR_LOG("{}: Terrain Frame constant member layout is incompatible",program); return false; }
        }
    }
    struct Input { const char* semantic; uint32_t index,location,type,count; };
    radray::vector<Input> inputs;
    if(!screen) {
        inputs={{"POSITION",0,0,1,3},{"COLOR",0,1,1,4},{"TEXCOORD",0,2,1,2}};
        if(!alpha) inputs.push_back({"TEXCOORD",1,3,3,2});
        inputs.push_back({"TEXCOORD",2,alpha?3u:4u,2,3});
        if(!alpha) inputs.push_back({"TEXCOORD",3,5,1,1});
    }
    if(artifact.VertexInputs().size()!=std::size(inputs)) { RADRAY_ERR_LOG("{}: Terrain vertex input count is incompatible",program); return false; }
    for(const auto& input:inputs) {
        auto found=std::find_if(artifact.VertexInputs().begin(),artifact.VertexInputs().end(),[&](const auto& value){
            return artifact.GetName(value.Semantic)==input.semantic && value.SemanticIndex==input.index;
        });
        if(found==artifact.VertexInputs().end() || (vk && found->Location!=input.location) ||
           found->ComponentType!=input.type || found->ComponentCount!=input.count) { RADRAY_ERR_LOG("{}: Terrain vertex input layout is incompatible",program); return false; }
    }
    return true;
}
}
