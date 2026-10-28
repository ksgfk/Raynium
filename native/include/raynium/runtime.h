#pragma once

#include <string_view>
#include <radray/render/rhi.h>
#include <radray/render/external_resource.h>

namespace raynium {

class RayniumEnvironment;

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
    // environment must outlive the runtime; its factory/instance is only borrowed.
    [[nodiscard]] static bool Create(RayniumEnvironment& environment,
                                    radray::unique_ptr<RayniumRuntime>& output,
                                    std::string_view& error,
                                    const std::optional<radray::render::ExternalDeviceIdentity>& host = {}) noexcept;

    radray::render::Device& GetDevice() noexcept { return *_device; }
    radray::render::CommandQueue& GetQueue() noexcept { return *_directQueue; }

private:
    RayniumRuntime(radray::shared_ptr<radray::render::Device> device,
                   radray::render::CommandQueue* directQueue) noexcept;

    radray::shared_ptr<radray::render::Device> _device;
    radray::render::CommandQueue* _directQueue;
};

}  // namespace raynium
