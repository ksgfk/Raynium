#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/solid_pipeline.h>
#include <radray/runtime/frame_scheduler.h>
#include <span>
#include <utility>

namespace raynium {
class Renderer final {
public:
    ~Renderer() noexcept;
    static bool Create(radray::DeviceContext&,std::span<const radray::byte>,uint32_t flags,radray::unique_ptr<Renderer>&);
    bool Present(Swapchain&,const rn_render_frame_desc&,rn_present_result&);
    bool PrepareOutput(radray::render::TextureFormat format) {
        return _pipeline->PrepareOutput(format,_device.GetDevice());
    }
    void Finish();
    bool CreateTexture(const rn_uploaded_texture_desc& desc) { return _uploaded.Create(_device,desc); }
    bool UploadTexture(const rn_texture_upload_desc& desc) { return _uploaded.Upload(desc); }
    bool DestroyTexture(uint32_t id) { return _uploaded.Destroy(id); }
    radray::DeviceContext& Runtime() { return _device; }
private:
    Renderer(radray::DeviceContext& device,radray::unique_ptr<radray::FrameScheduler> scheduler,
        radray::unique_ptr<radray::VertexBufferPool> vertices,radray::unique_ptr<radray::IndexBufferPool> indices,SolidFramePipeline* pipeline)
        :_device(device),_scheduler(std::move(scheduler)),_terrain(device,std::move(vertices),std::move(indices)),_pipeline(pipeline) {}
    radray::DeviceContext& _device;
    radray::unique_ptr<radray::FrameScheduler> _scheduler;
    TerrainResidency _terrain;
    SolidFramePipeline* _pipeline;
    UploadedTextures _uploaded;
};
}
