#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <utility>

namespace raynium {
void QueryDeviceInfo(radray::DeviceContext&,rn_device_info&);
using RenderBuffer = radray::unique_ptr<radray::gpu::Buffer>;
RenderBuffer CreateBuffer(radray::gpu::Device&, uint64_t, radray::gpu::MemoryType, radray::gpu::BufferUses,
                          radray::gpu::ResourceHints = {});

void TextureBarrier(radray::gpu::CommandBuffer&, radray::gpu::Texture*, radray::gpu::TextureStates,
                    radray::gpu::TextureStates);
void BufferBarrier(radray::gpu::CommandBuffer&, radray::gpu::Buffer*, radray::gpu::BufferStates, radray::gpu::BufferStates);
class Swapchain final {
public:
    static bool Create(radray::DeviceContext&,const rn_swapchain_desc&,radray::unique_ptr<Swapchain>&);
    bool Resize(const rn_swapchain_desc&);
    bool IsPresentable() const noexcept;
    bool Validate(uint64_t sequence) const;
    radray::gpu::SwapChain& Chain() const noexcept { return *_chain; }
    void Commit(uint64_t sequence) noexcept { _submitted=sequence; }
    uint64_t Timeouts() const noexcept { return _timeouts; }
    void TimedOut() noexcept { ++_timeouts; }
private:
    Swapchain(radray::unique_ptr<radray::gpu::SwapChain> chain,uint64_t hwnd)
        :_chain(std::move(chain)),_hwnd(hwnd) {}
    radray::unique_ptr<radray::gpu::SwapChain> _chain;
    uint64_t _hwnd{},_submitted{},_timeouts{};
};


}
