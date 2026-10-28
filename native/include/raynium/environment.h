#pragma once

#include <string_view>
#include <thread>
#include <radray/render/rhi.h>

namespace raynium {

// See doc/architecture/native-runtime.md for environment and device lifetimes.
class RayniumEnvironment final {
public:
    ~RayniumEnvironment() noexcept;
    RayniumEnvironment(const RayniumEnvironment&) = delete;
    RayniumEnvironment& operator=(const RayniumEnvironment&) = delete;
    RayniumEnvironment(RayniumEnvironment&&) = delete;
    RayniumEnvironment& operator=(RayniumEnvironment&&) = delete;

    // output must be empty; failure leaves it unchanged. Creates no GPU device.
    [[nodiscard]] static bool Create(radray::render::RenderBackend backend, bool validation,
                                    radray::unique_ptr<RayniumEnvironment>& output,
                                    std::string_view& error) noexcept;

    radray::render::RenderBackend GetBackend() const noexcept { return _backend; }
    bool IsValidationEnabled() const noexcept { return _validation; }
    bool IsOwnerThread() const noexcept { return _owner == std::this_thread::get_id(); }
    // Only use the accessor for the selected backend. Borrowed until environment destruction.
    radray::render::DXGIFactory& GetFactory() noexcept { return *_factory; }
    radray::render::InstanceVulkan& GetVulkanInstance() noexcept { return *_instance; }

private:
    RayniumEnvironment(radray::render::RenderBackend backend, bool validation,
                       radray::unique_ptr<radray::render::DXGIFactory> factory,
                       radray::render::InstanceVulkan* instance) noexcept;

    radray::render::RenderBackend _backend;
    bool _validation;
    std::thread::id _owner;
    radray::unique_ptr<radray::render::DXGIFactory> _factory;
    radray::render::InstanceVulkan* _instance;
};

}  // namespace raynium
