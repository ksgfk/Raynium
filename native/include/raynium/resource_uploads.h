#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <unordered_set>

#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/render_resources.h>
#include <raynium/resource_handles.h>

namespace raynium {
radray::gpu::TextureFormat ToTextureFormat(rn_pixel_format format) noexcept;

class FrameTextureImports final {
public:
    FrameTextureImports(
        radray::RDGBuilder& graph,
        std::pmr::memory_resource& scratch,
        const radray::gpu::DeviceLimits* limits = nullptr);
    radray::RDGTextureHandle Import(const radray::IntrusivePtr<radray::GpuTexture>& texture, bool overwritesWholeSubresource = false);
    radray::RDGTextureHandle Import(const rn_texture* texture, bool overwrite = false);
    std::pmr::memory_resource* Scratch() const noexcept;

private:
    radray::RDGBuilder& _graph;
    const radray::gpu::DeviceLimits* _limits{};
    std::pmr::unordered_set<radray::GpuTexture*> _cleared;
};

radray::IntrusivePtr<radray::GpuTexture> CreateSceneTexture(radray::gpu::Device& device, const rn_texture_desc& desc);
radray::gpu::TextureView* GetSamplingView(radray::gpu::Device& device, radray::GpuTexture& owner, const rn_texture_sampling& sampling);
radray::gpu::Sampler* GetTextureSampler(radray::gpu::Device& device, uint32_t bits, const rn_texture_sampling& sampling);

struct PendingTextureUpload {
    radray::IntrusivePtr<radray::GpuTexture> Target;
    rn_texture_upload_desc Desc{};
    radray::vector<radray::byte> Bytes;
};

struct PendingBufferUpload {
    radray::IntrusivePtr<radray::GpuBuffer> Target;
    radray::vector<radray::byte> Bytes;
};

struct ShadowWrite {
    radray::IntrusivePtr<radray::GpuBuffer> Target;
    uint64_t Offset{};
    std::span<const radray::byte> Bytes;
};

class PendingResourceOps final {
public:
    radray::vector<PendingTextureUpload> TextureUploads;
    radray::vector<PendingBufferUpload> BufferUploads;
    radray::vector<int32_t> RemovedModelMeshes;

    void AddUploads(radray::RDGBuilder& graph, FrameTextureImports& imports, std::span<const ShadowWrite> shadows);
    void Clear() noexcept;
};

bool ValidateTextureDesc(const radray::gpu::GpuDeviceCapabilities& caps, const rn_texture_desc& desc);
bool PrepareTextureUpload(const rn_texture& handle, const rn_texture_upload_desc& desc, PendingTextureUpload& pending);
}  // namespace raynium
