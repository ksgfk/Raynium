#include <raynium/render_resources.h>
#include <radray/types.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <utility>

namespace raynium {
using namespace radray::render;
RenderBuffer CreateBuffer(Device& d,uint64_t bytes,MemoryType memory,BufferUses uses,ResourceHints hints) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Buffer.Create");
    if(memory==MemoryType::Upload) uses|=BufferUse::MapWrite;
    return d.CreateBuffer({.Size=bytes,.Memory=memory,.Usage=uses,.Hints=hints}).Release();
}
void TextureBarrier(CommandBuffer& c,Texture* texture,TextureStates before,TextureStates after) {
    const ResourceBarrierDescriptor b=BarrierTextureDescriptor{.Target=texture,.Before=before,.After=after,
        .BeforeStages=ShaderStage::Vertex|ShaderStage::Pixel,.AfterStages=ShaderStage::Vertex|ShaderStage::Pixel};
    c.ResourceBarrier({&b,1});
}
void BufferBarrier(CommandBuffer& c,Buffer* buffer,BufferStates before,BufferStates after) {
    const ResourceBarrierDescriptor b=BarrierBufferDescriptor{.Target=buffer,.Before=before,.After=after}; c.ResourceBarrier({&b,1});
}
namespace {
bool ValidSwapchainDesc(const rn_swapchain_desc& desc) {
    if(desc.struct_size!=sizeof(desc) || !desc.width || !desc.height || desc.width>16384 || desc.height>16384 ||
       desc.present_mode>RN_PRESENT_FIFO || !IsWindow(reinterpret_cast<HWND>(desc.hwnd))) {
        RADRAY_ERR_LOG("Invalid swapchain descriptor, dimensions, present mode or HWND"); return false;
    }
    return true;
}
PresentMode Mode(rn_present_mode mode) { return mode==RN_PRESENT_FIFO?PresentMode::FIFO:PresentMode::Immediate; }

}
bool Swapchain::Create(radray::DeviceContext& runtime,const rn_swapchain_desc& desc,radray::unique_ptr<Swapchain>& out) {
    if(out) RADRAY_ABORT("Swapchain output occupied");
    if(!ValidSwapchainDesc(desc)) return false;
    auto& device=runtime.GetDevice();
    if(!device.QueryTextureSupport({TextureDimension::Dim2D,TextureFormat::BGRA8_UNORM,TextureUse::RenderTarget}).Supported) {
        RADRAY_ERR_LOG("Device does not support BGRA8 swapchain output"); return false;
    }
    auto chain=device.CreateSwapChain({.PresentQueue=&runtime.GetQueue(),.NativeHandler=reinterpret_cast<void*>(desc.hwnd),
        .Width=desc.width,.Height=desc.height,.BackBufferCount=3,.Format=TextureFormat::BGRA8_UNORM,.PresentMode=Mode(desc.present_mode)}).Release();
    if(!chain) return false;
    if(chain->GetDesc().Format!=TextureFormat::BGRA8_UNORM) RADRAY_ABORT("Swapchain did not retain BGRA8 output format");
    out=radray::unique_ptr<Swapchain>(new Swapchain(std::move(chain),desc.hwnd)); return true;
}
bool Swapchain::Resize(const rn_swapchain_desc& desc) {
    if(!ValidSwapchainDesc(desc)) return false;
    if(desc.hwnd!=_hwnd) { RADRAY_ERR_LOG("Swapchain HWND cannot change"); return false; }
    if(!_chain->Recreate(desc.width,desc.height,TextureFormat::BGRA8_UNORM,Mode(desc.present_mode))) {
        RADRAY_ERR_LOG("Cannot recreate BGRA8 swapchain; close the renderer"); return false;
    }
    return true;
}
bool Swapchain::IsPresentable() const noexcept {
    const auto window=reinterpret_cast<HWND>(_hwnd); RECT bounds{};
    return IsWindowVisible(window) && !IsIconic(window) && GetClientRect(window,&bounds) && bounds.right>bounds.left && bounds.bottom>bounds.top;
}
bool Swapchain::Validate(uint64_t sequence) const {
    if(!IsWindow(reinterpret_cast<HWND>(_hwnd)) || !sequence || sequence==UINT64_MAX || sequence!=_submitted+1) {
        RADRAY_ERR_LOG("Invalid swapchain HWND or output sequence (must start at 1 and increase by one)"); return false;
    }
    return true;
}


}
