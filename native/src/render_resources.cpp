#include <raynium/render_resources.h>

#include <algorithm>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/types.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace raynium {
bool ValidSwapchainDesc(const rn_swapchain_desc& desc) {
    if (desc.struct_size != sizeof(desc) || !desc.width || !desc.height || desc.width > 16384 || desc.height > 16384 ||
        desc.present_mode > RN_PRESENT_FIFO || !IsWindow(reinterpret_cast<HWND>(desc.hwnd))) {
        RADRAY_ERR_LOG("Invalid swapchain descriptor, dimensions, present mode or HWND");
        return false;
    }
    return true;
}

namespace {
radray::gpu::PresentMode ToPresentMode(rn_present_mode mode) {
    return mode == RN_PRESENT_FIFO ? radray::gpu::PresentMode::FIFO : radray::gpu::PresentMode::Immediate;
}
}  // namespace

Swapchain::Swapchain(radray::unique_ptr<radray::gpu::SwapChain> chain, uint64_t hwnd)
    : _chain(std::move(chain)), _hwnd(hwnd) {}

bool Swapchain::Create(radray::DeviceContext& runtime, const rn_swapchain_desc& desc, radray::unique_ptr<Swapchain>& out) {
    if (out) RADRAY_ABORT("Swapchain output occupied");
    if (!ValidSwapchainDesc(desc)) return false;
    auto& device = runtime.GetDevice();
    if (!device.QueryTextureSupport({radray::gpu::TextureDimension::Dim2D, radray::gpu::TextureFormat::BGRA8_UNORM, radray::gpu::TextureUse::RenderTarget}).Supported) {
        RADRAY_ERR_LOG("Device does not support BGRA8 swapchain output");
        return false;
    }
    auto chain = device.CreateSwapChain({
                                            .PresentQueue = &runtime.GetQueue(),
                                            .NativeHandler = reinterpret_cast<void*>(desc.hwnd),
                                            .Width = desc.width,
                                            .Height = desc.height,
                                            .BackBufferCount = 3,
                                            .Format = radray::gpu::TextureFormat::BGRA8_UNORM,
                                            .PresentMode = ToPresentMode(desc.present_mode),
                                        })
                     .Release();
    if (!chain) return false;
    if (chain->GetDesc().Format != radray::gpu::TextureFormat::BGRA8_UNORM) RADRAY_ABORT("Swapchain did not retain BGRA8 output format");
    out = radray::unique_ptr<Swapchain>(new Swapchain(std::move(chain), desc.hwnd));
    return true;
}

bool Swapchain::Resize(const rn_swapchain_desc& desc) {
    if (!ValidSwapchainDesc(desc)) return false;
    if (desc.hwnd != _hwnd) {
        RADRAY_ERR_LOG("Swapchain HWND cannot change");
        return false;
    }
    _broken = !_chain->Recreate(desc.width, desc.height, radray::gpu::TextureFormat::BGRA8_UNORM, ToPresentMode(desc.present_mode));
    if (_broken) RADRAY_ERR_LOG("Cannot recreate BGRA8 swapchain");
    return !_broken;
}

bool Swapchain::IsBroken() const noexcept {
    return _broken;
}

void Swapchain::MarkBroken() noexcept {
    _broken = true;
}

bool Swapchain::IsPresentable() const noexcept {
    const auto window = reinterpret_cast<HWND>(_hwnd);
    RECT bounds{};
    return IsWindowVisible(window) && !IsIconic(window) && GetClientRect(window, &bounds) && bounds.right > bounds.left && bounds.bottom > bounds.top;
}

bool Swapchain::Validate() const {
    if (!IsWindow(reinterpret_cast<HWND>(_hwnd))) {
        RADRAY_ERR_LOG("Invalid swapchain HWND");
        return false;
    }
    return true;
}

radray::gpu::SwapChain& Swapchain::Chain() const noexcept {
    return *_chain;
}
}  // namespace raynium
