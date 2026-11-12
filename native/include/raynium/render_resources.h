#pragma once
#include <radray/types.h>
#include <radray/runtime/gpu_device.h>
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <utility>

namespace raynium {
void QueryDeviceInfo(radray::DeviceContext&,rn_device_info&);
using RenderBuffer = radray::unique_ptr<radray::render::Buffer>;
RenderBuffer CreateBuffer(radray::render::Device&, uint64_t, radray::render::MemoryType, radray::render::BufferUses,
                          radray::render::ResourceHints = {});

void TextureBarrier(radray::render::CommandBuffer&, radray::render::Texture*, radray::render::TextureStates,
                    radray::render::TextureStates);
void BufferBarrier(radray::render::CommandBuffer&, radray::render::Buffer*, radray::render::BufferStates, radray::render::BufferStates);
class Swapchain final {
public:
    static bool Create(radray::DeviceContext&,const rn_swapchain_desc&,radray::unique_ptr<Swapchain>&);
    bool Resize(const rn_swapchain_desc&);
    bool IsPresentable() const noexcept;
    bool Validate(uint64_t sequence) const;
    radray::render::SwapChain& Chain() const noexcept { return *_chain; }
    void Commit(uint64_t sequence) noexcept { _submitted=sequence; }
    uint64_t Timeouts() const noexcept { return _timeouts; }
    void TimedOut() noexcept { ++_timeouts; }
private:
    Swapchain(radray::unique_ptr<radray::render::SwapChain> chain,uint64_t hwnd)
        :_chain(std::move(chain)),_hwnd(hwnd) {}
    radray::unique_ptr<radray::render::SwapChain> _chain;
    uint64_t _hwnd{},_submitted{},_timeouts{};
};


}
