#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <utility>

namespace raynium {
void QueryDeviceInfo(radray::DeviceContext&,rn_device_info&);
bool ValidSwapchainDesc(const rn_swapchain_desc& desc);
class Swapchain final {
public:
    static bool Create(radray::DeviceContext&,const rn_swapchain_desc&,radray::unique_ptr<Swapchain>&);
    bool Resize(const rn_swapchain_desc&);
    bool IsBroken() const noexcept { return _broken; }
    void MarkBroken() noexcept { _broken=true; }
    bool IsPresentable() const noexcept;
    bool Validate() const;
    radray::gpu::SwapChain& Chain() const noexcept { return *_chain; }
private:
    Swapchain(radray::unique_ptr<radray::gpu::SwapChain> chain,uint64_t hwnd)
        :_chain(std::move(chain)),_hwnd(hwnd) {}
    radray::unique_ptr<radray::gpu::SwapChain> _chain;
    bool _broken{};
    uint64_t _hwnd{};
};


}
