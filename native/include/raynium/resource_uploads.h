#pragma once
#include <raynium/resource_handles.h>
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
    std::pmr::memory_resource* Scratch() const { return _cleared.get_allocator().resource(); }
    radray::RDGTextureHandle Import(const rn_texture* texture,bool overwrite=false) {
        return Import(texture->texture,overwrite);
    }
private:
    radray::RDGBuilder& _graph;
    const radray::gpu::DeviceDetail* _detail;
    std::pmr::unordered_set<radray::GpuTexture*> _cleared;
};
radray::IntrusivePtr<radray::GpuTexture> CreateSceneTexture(radray::gpu::Device&,const rn_texture_desc&);
radray::gpu::TextureView* GetSamplingView(radray::gpu::Device&,radray::GpuTexture&,const rn_texture_sampling&);
radray::gpu::Sampler* GetTextureSampler(radray::gpu::Device&,uint32_t,const rn_texture_sampling&);
struct PendingTextureUpload {
    radray::IntrusivePtr<radray::GpuTexture> target;
    rn_texture_upload_desc desc;
    radray::vector<radray::byte> bytes;
};
struct PendingBufferUpload {
    radray::IntrusivePtr<radray::GpuBuffer> target;
    radray::vector<radray::byte> bytes;
};
struct ShadowWrite {
    radray::IntrusivePtr<radray::GpuBuffer> target;
    uint64_t offset;
    std::span<const radray::byte> bytes;
};
struct PendingResourceOps {
    radray::vector<PendingTextureUpload> textureUploads;
    radray::vector<PendingBufferUpload> bufferUploads;
    radray::vector<int32_t> removedModelMeshes;
    void AddUploads(radray::RDGBuilder&,FrameTextureImports&,std::span<const ShadowWrite>);
};
bool ValidateTextureDesc(const radray::gpu::GpuDeviceCapabilities&,const rn_texture_desc&);
bool PrepareTextureUpload(const rn_texture&,const rn_texture_upload_desc&,PendingTextureUpload&);
}
