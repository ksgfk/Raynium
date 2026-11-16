#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/render_resources.h>
#include <radray/runtime/gpu_resource.h>

namespace raynium {
class TextureResource final : public radray::RefCountedBase<TextureResource> {
public:
    static radray::IntrusivePtr<TextureResource> Create(radray::render::Device&,const rn_uploaded_texture_desc&);
    radray::render::Texture* GetTexture() const noexcept { return _uploaded?_uploaded->GetTexture():_texture.get(); }
    radray::render::TextureView* GetDefaultView() const noexcept { return _uploaded?_uploaded->GetDefaultView():_view.get(); }
    radray::render::TextureView* GetSamplingView(radray::render::Device&,const rn_texture_sampling&);
    radray::RDGTextureHandle Import(radray::RDGBuilder&,radray::FrameSlot&,radray::vector<TextureResource*>&);
    void PublishSubmittedState() noexcept;
    ~TextureResource() noexcept override = default;
private:
    explicit TextureResource(radray::IntrusivePtr<radray::GpuTexture> uploaded):_uploaded(std::move(uploaded)) {}
    TextureResource(radray::unique_ptr<radray::render::Texture> texture,radray::unique_ptr<radray::render::TextureView> view)
        :_texture(std::move(texture)),_view(std::move(view)) {}
    radray::IntrusivePtr<radray::GpuTexture> _uploaded;
    radray::unique_ptr<radray::render::Texture> _texture;
    radray::unique_ptr<radray::render::TextureView> _view;
    radray::unordered_map<uint64_t,radray::unique_ptr<radray::render::TextureView>> _samplingViews;
    radray::render::TextureState _state{radray::render::TextureState::Undefined};
    uint64_t _frameId{UINT64_MAX};
    radray::RDGTextureHandle _handle;
};
radray::render::Sampler* GetTextureSampler(radray::render::Device&,
    radray::unordered_map<uint64_t,radray::unique_ptr<radray::render::Sampler>>&,uint32_t,const rn_texture_sampling&);
struct UploadedTexture {
    rn_uploaded_texture_desc desc{};
    radray::IntrusivePtr<TextureResource> texture;
};
class UploadedTextures final {
public:
    bool Create(radray::DeviceContext&,const rn_uploaded_texture_desc&);
    bool Upload(const rn_texture_upload_desc&);
    bool Destroy(uint32_t);
    const UploadedTexture* Find(uint32_t) const;
    // Transfers pending CPU payloads and GPU owners into the graph, including upload-only frames.
    void AddUploads(radray::RDGBuilder&,radray::FrameSlot&,radray::vector<TextureResource*>&,rn_frame_metrics*);
private:
    struct Pending { radray::IntrusivePtr<TextureResource> target; rn_texture_upload_desc desc; radray::vector<radray::byte> bytes; };
    radray::unordered_map<uint32_t,UploadedTexture> _textures;
    radray::unordered_set<uint32_t> _used;
    radray::vector<Pending> _pending;
};
}
