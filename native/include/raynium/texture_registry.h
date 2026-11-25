#pragma once
#include <memory_resource>
#include <unordered_set>
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/render_resources.h>
#include <radray/runtime/gpu_resource.h>

namespace raynium {
radray::gpu::TextureFormat ToTextureFormat(rn_pixel_format) noexcept;
class FrameTextureImports final {
public:
    FrameTextureImports(radray::RDGBuilder& graph,std::pmr::memory_resource& scratch,const radray::gpu::DeviceDetail* detail=nullptr);
    radray::RDGTextureHandle Import(const radray::IntrusivePtr<radray::GpuTexture>& texture,bool overwritesWholeSubresource=false);
private:
    radray::RDGBuilder& _graph;
    const radray::gpu::DeviceDetail* _detail;
    std::pmr::unordered_set<radray::GpuTexture*> _cleared;
};
radray::IntrusivePtr<radray::GpuTexture> CreateSceneTexture(radray::gpu::Device&,const rn_uploaded_texture_desc&);
radray::gpu::TextureView* GetSamplingView(radray::gpu::Device&,radray::GpuTexture&,const rn_texture_sampling&);
radray::gpu::Sampler* GetTextureSampler(radray::gpu::Device&,uint32_t,const rn_texture_sampling&);
struct UploadedTexture {
    rn_uploaded_texture_desc desc{};
    radray::IntrusivePtr<radray::GpuTexture> texture;
};
class TextureRegistry final {
public:
    bool Create(radray::DeviceContext&,const rn_uploaded_texture_desc&);
    struct Upload { rn_texture_upload_desc desc; radray::vector<radray::byte> bytes; };
    static bool PrepareUpload(const rn_uploaded_texture_desc&,const rn_texture_upload_desc&,Upload&);
    void Enqueue(Upload upload);
    bool Destroy(uint32_t);
    const UploadedTexture* Find(uint32_t) const;
    // Transfers pending CPU payloads and GPU owners into the graph, including upload-only frames.
    void AddUploads(radray::RDGBuilder&,FrameTextureImports&,rn_frame_metrics*);
private:
    struct Pending { radray::IntrusivePtr<radray::GpuTexture> target; rn_texture_upload_desc desc; radray::vector<radray::byte> bytes; };
    radray::unordered_map<uint32_t,UploadedTexture> _textures;
    radray::unordered_set<uint32_t> _used;
    radray::vector<Pending> _pending;
};
}
