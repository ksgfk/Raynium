#include <raynium/texture_resources.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace raynium {
using namespace radray::render;
bool UploadedTextures::Create(radray::DeviceContext& runtime,const rn_uploaded_texture_desc& desc) {
    uint32_t levels=1;
    for(uint32_t side=std::max(desc.width,desc.height);side>1;side>>=1) ++levels;
    auto& device=runtime.GetDevice();
    if(desc.struct_size!=sizeof(desc) || desc.id<16 || _used.contains(desc.id) || desc.format>RN_PIXEL_R8 ||
       !desc.width || !desc.height || desc.width>device.GetCapabilities().Limits.MaxTexture2DDimension ||
       desc.height>device.GetCapabilities().Limits.MaxTexture2DDimension || !desc.mip_levels || desc.mip_levels>levels) {
        RADRAY_ERR_LOG("Invalid uploaded texture descriptor, format or reused ID: id={} size={}x{} mips={} format={}",desc.id,desc.width,desc.height,desc.mip_levels,uint32_t(desc.format)); return false;
    }
    const auto format=desc.format==RN_PIXEL_R8?TextureFormat::R8_UNORM:TextureFormat::RGBA8_UNORM;
    const auto usage=TextureUse::CopyDestination|TextureUse::Resource;
    if(!device.QueryTextureSupport({TextureDimension::Dim2D,format,usage}).Supported) { RADRAY_ERR_LOG("Uploaded texture format unsupported: id={} format={}",desc.id,uint32_t(desc.format)); return false; }
    UploadedTexture value{desc,radray::GpuTexture::Create(device,{.Dim=TextureDimension::Dim2D,.Width=desc.width,.Height=desc.height,
        .ArrayLayers=1,.MipLevels=desc.mip_levels,.Format=format}).Release()};
    if(!value.texture) { RADRAY_ERR_LOG("Cannot create uploaded texture: id={}",desc.id); return false; }
    _textures.emplace(desc.id,std::move(value)); _used.insert(desc.id); return true;
}
const UploadedTexture* UploadedTextures::Find(uint32_t id) const {
    auto found=_textures.find(id); return found==_textures.end()?nullptr:&found->second;
}
bool UploadedTextures::Upload(const rn_texture_upload_desc& desc) {
    const auto* owner=Find(desc.id);
    if(!owner || desc.struct_size!=sizeof(desc) || !desc.pixels || desc.mip_level>=owner->desc.mip_levels || !desc.width || !desc.height) {
        RADRAY_ERR_LOG("Invalid upload descriptor or missing texture: id={} mip={}",desc.id,desc.mip_level); return false;
    }
    const uint32_t width=std::max(1u,owner->desc.width>>desc.mip_level),height=std::max(1u,owner->desc.height>>desc.mip_level);
    const uint64_t rowBytes=uint64_t(desc.width)*(owner->desc.format==RN_PIXEL_R8?1u:4u);
    const uint64_t pitch=desc.row_pitch?desc.row_pitch:rowBytes;
    if(uint64_t(desc.x)+desc.width>width || uint64_t(desc.y)+desc.height>height || pitch<rowBytes ||
       desc.byte_size<pitch*(desc.height-1)+rowBytes) { RADRAY_ERR_LOG("Texture upload region, row pitch or byte size is invalid: id={} mip={} region={},{},{}x{} row_pitch={} byte_size={}",desc.id,desc.mip_level,desc.x,desc.y,desc.width,desc.height,pitch,desc.byte_size); return false; }
    Pending pending{owner->texture,desc,{}};
    pending.bytes.resize(size_t(rowBytes*desc.height));
    for(uint32_t row=0;row<desc.height;++row)
        std::memcpy(pending.bytes.data()+rowBytes*row,static_cast<const radray::byte*>(desc.pixels)+pitch*row,size_t(rowBytes));
    pending.desc.pixels=nullptr; pending.desc.row_pitch=uint32_t(rowBytes); pending.desc.byte_size=pending.bytes.size();
    _pending.push_back(std::move(pending)); return true;
}
bool UploadedTextures::Destroy(uint32_t id) {
    if(!_textures.erase(id)) { RADRAY_ERR_LOG("Texture ID is missing or already destroyed: {}",id); return false; }
    return true;
}
void UploadedTextures::AddUploads(radray::RDGBuilder& graph) {
    for(auto& upload:_pending) {
        const auto target=graph.ImportPersistentOwned(upload.target);
        graph.UploadTexture(target,{upload.desc.mip_level,0,upload.desc.x,upload.desc.y,upload.desc.width,upload.desc.height},
            radray::RDGTextureUploadSource::OwnedBytes(std::move(upload.bytes),upload.desc.row_pitch));
        graph.MarkOutput(target);
    }
    _pending.clear();
}
}
