#include <raynium/environment.h>
#include <new>
#include <utility>
#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace raynium {

using namespace radray::render;

RayniumEnvironment::RayniumEnvironment(RenderBackend backend, bool validation,
                                       radray::unique_ptr<DXGIFactory> factory,
                                       InstanceVulkan* instance) noexcept
    : _backend(backend), _validation(validation), _owner(std::this_thread::get_id()),
      _factory(std::move(factory)), _instance(instance) {}

RayniumEnvironment::~RayniumEnvironment() noexcept {
    _factory.reset();
    if (_instance) InstanceVulkan::ShutdownEnv();
    RADRAY_INFO_LOG("RayniumEnvironment: destroyed");
}

bool RayniumEnvironment::Create(RenderBackend backend, bool validation,
                                radray::unique_ptr<RayniumEnvironment>& output,
                                std::string_view& error) noexcept {
    error = {};
    if (output) {
        error = "RayniumEnvironment Create output must be empty";
        return false;
    }
    InstanceVulkan* instance = nullptr;
    auto cleanup = radray::MakeScopeGuard([&]() noexcept {
        if (instance) InstanceVulkan::ShutdownEnv();
    });
    radray::unique_ptr<DXGIFactory> factory;
    switch (backend) {
        case RenderBackend::D3D12: {
            RADRAY_INFO_LOG("RayniumEnvironment: creating DXGI factory");
            DXGIFactoryDescriptor desc{};
            desc.IsEnableDebugLayer = validation;
            if (validation) {
                auto diagnostics = DXGIDiagnosticContext::Create();
                if (!diagnostics) {
                    error = "D3D12: cannot create diagnostic context";
                    return false;
                }
                desc.DiagnosticContext = diagnostics.Release();
            }
            auto created = DXGIFactory::Create(desc);
            if (!created) {
                error = "D3D12: DXGI factory creation failed; see preceding RadRay diagnostics";
                return false;
            }
            factory = created.Release();
            break;
        }
        case RenderBackend::Vulkan: {
            RADRAY_INFO_LOG("RayniumEnvironment: creating Vulkan environment");
            VulkanInstanceDescriptor desc{};
            desc.AppName = "Raynium";
            desc.EngineName = "RayniumRuntime";
            desc.IsEnableDebugLayer = validation;
            desc.IsEnableSynchronizationValidation = validation;
            auto created = InstanceVulkan::InitEnv(desc);
            if (!created) {
                error = "Vulkan: environment creation failed (loader, instance or active environment)";
                return false;
            }
            instance = created.Get();
            break;
        }
        default:
            error = "Unsupported RHI backend";
            return false;
    }
    radray::unique_ptr<RayniumEnvironment> environment{new (std::nothrow) RayniumEnvironment{
        backend, validation, std::move(factory), instance}};
    if (!environment) {
        error = "Cannot allocate RayniumEnvironment";
        return false;
    }
    cleanup.Dismiss();
    RADRAY_INFO_LOG("RayniumEnvironment: ready backend={} driver validation={}", backend, validation);
    output = std::move(environment);
    return true;
}

}  // namespace raynium
