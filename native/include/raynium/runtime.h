#pragma once

#include <string_view>
#include <radray/render/rhi.h>

namespace raynium {

// See doc/architecture/native-runtime.md for the runtime ownership contract.
class RayniumRuntime final {
public:
    ~RayniumRuntime() noexcept;

    RayniumRuntime(const RayniumRuntime&) = delete;
    RayniumRuntime& operator=(const RayniumRuntime&) = delete;
    RayniumRuntime(RayniumRuntime&&) = delete;
    RayniumRuntime& operator=(RayniumRuntime&&) = delete;

    // output must be empty. Success transfers a complete object and clears error;
    // failure leaves output unchanged and returns a diagnostic with static lifetime.
    [[nodiscard]] static bool Create(radray::render::RenderBackend backend,
                                    radray::unique_ptr<RayniumRuntime>& output,
                                    std::string_view& error) noexcept;

private:
    RayniumRuntime(radray::unique_ptr<radray::render::DXGIFactory> factory,
                   radray::shared_ptr<radray::render::Device> device,
                   radray::render::CommandQueue* directQueue,
                   bool ownsVulkanEnvironment) noexcept;

    radray::unique_ptr<radray::render::DXGIFactory> _factory;
    radray::shared_ptr<radray::render::Device> _device;
    radray::render::CommandQueue* _directQueue;
    bool _ownsVulkanEnvironment;
};

}  // namespace raynium
