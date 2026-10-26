#include <raynium/runtime.h>
#include <new>
#include <utility>
#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace raynium {

using namespace radray::render;

RayniumRuntime::RayniumRuntime(radray::unique_ptr<DXGIFactory> factory,
                               radray::shared_ptr<Device> device,
                               CommandQueue* directQueue,
                               bool ownsVulkanEnvironment) noexcept
    : _factory(std::move(factory)),
      _device(std::move(device)),
      _directQueue(directQueue),
      _ownsVulkanEnvironment(ownsVulkanEnvironment) {}

RayniumRuntime::~RayniumRuntime() noexcept {
    RADRAY_INFO_LOG("RayniumRuntime: waiting for Direct queue");
    _directQueue->Wait();
    _device.reset();
    _factory.reset();
    if (_ownsVulkanEnvironment) InstanceVulkan::ShutdownEnv();
}

bool RayniumRuntime::Create(RenderBackend backend, radray::unique_ptr<RayniumRuntime>& output,
                           std::string_view& error) noexcept {
    error = {};
    if (output) {
        error = "RayniumRuntime Create output must be empty";
        return false;
    }

    bool ownsVulkanEnvironment = false;
    // Declared before device/factory so failure destroys them before the environment.
    auto environmentCleanup = radray::MakeScopeGuard([&]() noexcept {
        if (ownsVulkanEnvironment) InstanceVulkan::ShutdownEnv();
    });
    radray::unique_ptr<DXGIFactory> factory;
    radray::shared_ptr<Device> device;
    RADRAY_INFO_LOG("RayniumRuntime: creating {}", backend);

    switch (backend) {
        case RenderBackend::D3D12: {
            RADRAY_INFO_LOG("RayniumRuntime: creating DXGI factory");
            auto createdFactory = DXGIFactory::Create({});
            if (!createdFactory) {
                error = "D3D12: DXGI factory creation failed";
                return false;
            }
            factory = createdFactory.Release();
            auto adapter = factory->SelectHighPerformanceAdapter();
            if (!adapter) {
                error = "D3D12: no suitable high-performance adapter";
                return false;
            }
            D3D12DeviceDescriptor desc{};
            desc.Factory = factory.get();
            desc.AdapterIndex = adapter;
            desc.QueueCounts = {1, 0, 0};
            RADRAY_INFO_LOG("RayniumRuntime: creating D3D12 device on adapter {}", *adapter);
            auto createdDevice = Device::Create(DeviceDescriptor{desc});
            if (!createdDevice) {
                error = "D3D12: device creation failed; see preceding RadRay diagnostics";
                return false;
            }
            device = createdDevice.Release();
            break;
        }
        case RenderBackend::Vulkan: {
            RADRAY_INFO_LOG("RayniumRuntime: creating Vulkan environment");
            VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "Raynium";
            instanceDesc.EngineName = "RayniumRuntime";
            auto instance = InstanceVulkan::InitEnv(instanceDesc);
            if (!instance) {
                error = "Vulkan: environment creation failed (loader, instance or active environment)";
                return false;
            }
            ownsVulkanEnvironment = true;
            auto adapter = instance->SelectHighPerformancePhysicalDevice();
            if (!adapter) {
                error = "Vulkan: no suitable high-performance physical device";
                return false;
            }
            const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
            VulkanDeviceDescriptor desc{};
            desc.PhysicalDeviceIndex = adapter;
            desc.Queues = queues;
            RADRAY_INFO_LOG("RayniumRuntime: creating Vulkan device on adapter {}", *adapter);
            auto createdDevice = Device::Create(DeviceDescriptor{desc});
            if (!createdDevice) {
                error = "Vulkan: device creation failed; see preceding RadRay diagnostics";
                return false;
            }
            device = createdDevice.Release();
            break;
        }
        default:
            error = "Unsupported RHI backend";
            return false;
    }

    auto directQueue = device->GetCommandQueue(QueueType::Direct, 0);
    if (!directQueue) {
        error = "RHI Direct queue is unavailable";
        return false;
    }
    radray::unique_ptr<RayniumRuntime> runtime{new (std::nothrow) RayniumRuntime{
        std::move(factory), std::move(device), directQueue.Get(), ownsVulkanEnvironment}};
    if (!runtime) {
        error = "Cannot allocate RayniumRuntime";
        return false;
    }
    environmentCleanup.Dismiss();
    RADRAY_INFO_LOG("RayniumRuntime: ready backend={} GPU={} Direct queues=1 RHI validation={}",
                   runtime->_device->GetBackend(), runtime->_device->GetDetail().GpuName, RhiValidationOptions{}.IsEnabled);
    output = std::move(runtime);
    return true;
}

}  // namespace raynium
