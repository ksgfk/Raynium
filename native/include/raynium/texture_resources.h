#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/render_resources.h>
#include <radray/runtime/gpu_resource.h>

namespace raynium {
struct UploadedTexture {
    rn_uploaded_texture_desc desc{};
    radray::IntrusivePtr<radray::GpuTexture> texture;
};
class UploadedTextures final {
public:
    bool Create(radray::DeviceContext&,const rn_uploaded_texture_desc&);
    bool Upload(const rn_texture_upload_desc&);
    bool Destroy(uint32_t);
    const UploadedTexture* Find(uint32_t) const;
    // Transfers pending CPU payloads and GPU owners into the graph, including upload-only frames.
    void AddUploads(radray::RDGBuilder&);
private:
    struct Pending { radray::IntrusivePtr<radray::GpuTexture> target; rn_texture_upload_desc desc; radray::vector<radray::byte> bytes; };
    radray::unordered_map<uint32_t,UploadedTexture> _textures;
    radray::unordered_set<uint32_t> _used;
    radray::vector<Pending> _pending;
};
}
