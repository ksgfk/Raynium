#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/render_resources.h>
#include <radray/runtime/upload_buffer.h>

namespace raynium {
struct UploadedTexture {
    rn_uploaded_texture_desc desc{};
    radray::unique_ptr<radray::render::Texture> texture;
    radray::unique_ptr<radray::render::TextureView> view;
    bool initialized{};
};
struct TextureCopy {
    radray::shared_ptr<UploadedTexture> owner;
    radray::render::BufferTextureCopyRegion region;
};
class UploadedTextures final {
public:
    bool Create(radray::DeviceContext&,const rn_uploaded_texture_desc&);
    bool Upload(const rn_texture_upload_desc&);
    bool Destroy(uint32_t);
    radray::shared_ptr<UploadedTexture> Find(uint32_t) const;
    // The frame slot owns copied resource references until its commands have been reset.
    bool Prepare(radray::render::Device&,radray::MappedUploadBuffer&,radray::vector<TextureCopy>&);
private:
    struct Pending { radray::shared_ptr<UploadedTexture> owner; rn_texture_upload_desc desc; radray::vector<radray::byte> bytes; };
    radray::unordered_map<uint32_t,radray::shared_ptr<UploadedTexture>> _textures;
    radray::unordered_set<uint32_t> _used;
    radray::vector<Pending> _pending;
};
}
