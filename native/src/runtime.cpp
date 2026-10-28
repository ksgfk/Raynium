#include <raynium/runtime.h>
#include <new>
#include <utility>
#include <radray/logger.h>
#include <raynium/environment.h>

namespace raynium {

using namespace radray::render;

namespace {
template<class Infos, class CreateDevice>
void MatchHost(radray::shared_ptr<Device>& device, const Infos& infos, uint32_t preferred,
               CreateDevice create, const std::optional<ExternalDeviceIdentity>& host) {
    if (!host) return;
    const auto matches = [&](Device& candidate) {
        auto extension = candidate.GetExternalResourceExtension();
        if (!extension) return false;
        auto identity = extension->GetDeviceIdentity();
        auto* value = std::get_if<ExternalDeviceIdentity>(&identity);
        return value && *value == *host;
    };
    if (matches(*device)) return;
    for (const auto& info : infos) {
        if (info.Index == preferred) continue;
        auto candidate = create(info.Index);
        if (candidate && matches(*candidate.Get())) {
            device = candidate.Release();
            return;
        }
    }
    RADRAY_WARN_LOG("Raynium: no external-resource adapter matches the host; preview will be unavailable");
}
}

RayniumRuntime::RayniumRuntime(radray::shared_ptr<Device> device,
                               CommandQueue* directQueue) noexcept
    : _device(std::move(device)), _directQueue(directQueue) {}

RayniumRuntime::~RayniumRuntime() noexcept {
    RADRAY_INFO_LOG("RayniumRuntime: waiting for Direct queue");
    _directQueue->Wait();
    _device.reset();
}

bool RayniumRuntime::Create(RayniumEnvironment& environment, radray::unique_ptr<RayniumRuntime>& output,
                           std::string_view& error, const std::optional<ExternalDeviceIdentity>& host) noexcept {
    error = {};
    if (output) {
        error = "RayniumRuntime Create output must be empty";
        return false;
    }

    const auto backend = environment.GetBackend();
    radray::shared_ptr<Device> device;
    RADRAY_INFO_LOG("RayniumRuntime: creating {}", backend);

    switch (backend) {
        case RenderBackend::D3D12: {
            auto* factory = &environment.GetFactory();
            auto adapter = factory->SelectHighPerformanceAdapter();
            if (!adapter) {
                error = "D3D12: no suitable high-performance adapter";
                return false;
            }
            D3D12DeviceDescriptor desc{};
            desc.Factory = factory;
            desc.AdapterIndex = adapter;
            desc.QueueCounts = {1, 0, 0};
            desc.EnableExternalResource = true;
            RADRAY_INFO_LOG("RayniumRuntime: creating D3D12 device on adapter {}", *adapter);
            auto createdDevice = Device::Create(DeviceDescriptor{desc});
            if (!createdDevice) {
                error = "D3D12: device creation failed; see preceding RadRay diagnostics";
                return false;
            }
            device = createdDevice.Release();
            MatchHost(device, factory->GetAdapters(), *adapter, [&](uint32_t index) {
                desc.AdapterIndex = index;
                return Device::Create(DeviceDescriptor{desc});
            }, host);
            break;
        }
        case RenderBackend::Vulkan: {
            auto* instance = &environment.GetVulkanInstance();
            auto adapter = instance->SelectHighPerformancePhysicalDevice();
            if (!adapter) {
                error = "Vulkan: no suitable high-performance physical device";
                return false;
            }
            const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
            VulkanDeviceDescriptor desc{};
            desc.PhysicalDeviceIndex = adapter;
            desc.Queues = queues;
            desc.EnableExternalResource = true;
            RADRAY_INFO_LOG("RayniumRuntime: creating Vulkan device on adapter {}", *adapter);
            auto createdDevice = Device::Create(DeviceDescriptor{desc});
            if (!createdDevice) {
                error = "Vulkan: device creation failed; see preceding RadRay diagnostics";
                return false;
            }
            device = createdDevice.Release();
            MatchHost(device, instance->GetPhysicalDevices(), *adapter, [&](uint32_t index) {
                desc.PhysicalDeviceIndex = index;
                return Device::Create(DeviceDescriptor{desc});
            }, host);
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
        std::move(device), directQueue.Get()}};
    if (!runtime) {
        error = "Cannot allocate RayniumRuntime";
        return false;
    }
    RADRAY_INFO_LOG("RayniumRuntime: ready backend={} GPU={} Direct queues=1 RHI validation={} driver validation={}",
                   runtime->_device->GetBackend(), runtime->_device->GetCapabilities().Detail.GpuName,
                   RhiValidationOptions{}.IsEnabled, environment.IsValidationEnabled());
    output = std::move(runtime);
    return true;
}

}  // namespace raynium
