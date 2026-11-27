#pragma once

#include <cstdint>

#include <radray/runtime/gpu_device.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/bridge.h>

namespace raynium {
bool ValidSwapchainDesc(const rn_swapchain_desc& desc);

class Swapchain final {
public:
    static bool Create(radray::DeviceContext& runtime, const rn_swapchain_desc& desc, radray::unique_ptr<Swapchain>& out);
    bool Resize(const rn_swapchain_desc& desc);
    bool IsBroken() const noexcept;
    void MarkBroken() noexcept;
    bool IsPresentable() const noexcept;
    bool Validate() const;
    radray::gpu::SwapChain& Chain() const noexcept;

private:
    Swapchain(radray::unique_ptr<radray::gpu::SwapChain> chain, uint64_t hwnd);

    radray::unique_ptr<radray::gpu::SwapChain> _chain;
    bool _broken{};
    uint64_t _hwnd{};
};
}  // namespace raynium
