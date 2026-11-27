// This layer runs in an isolated test worker with one live instance and device.
#define VK_NO_PROTOTYPES
#include <vulkan/vk_layer.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
PFN_vkGetInstanceProcAddr instanceProc{};
PFN_vkGetDeviceProcAddr deviceProc{};
PFN_GetPhysicalDeviceProcAddr physicalProc{};
VkInstance currentInstance{};
VkDevice currentDevice{};
VkDeviceSize coherentAtomSize{};
VkSemaphore batchGate{};
std::atomic<bool> armGate{}, maskCounter{}, dedicatedBuffers{};

enum Operation { None,
                 Timeline,
                 Image,
                 Submit,
                 Present,
                 Acquire,
                 MaskTimeline,
                 CreateSwapchain,
                 MapMemory,
                 DedicatedBuffers };
std::mutex gateMutex;
std::condition_variable gateChanged;
std::atomic<int> pauseSite{};
bool atGate{}, gateExpired{};
void checkpoint(Operation site) {
    std::unique_lock lock(gateMutex);
    if (pauseSite != site) return;
    atGate = true;
    gateChanged.notify_all();
    if (!gateChanged.wait_for(lock, std::chrono::seconds(10), [] { return pauseSite == None; })) {
        pauseSite = None;
        gateExpired = true;
    }
    atGate = false;
    gateChanged.notify_all();
}
std::atomic<Operation> armed{};
std::atomic<VkResult> injected{}, createError{}, mapError{};
std::atomic<int> hits{}, acquisitions{}, presentations{}, idleCalls{}, failedCreates{}, counterReads{};

bool take(Operation operation) {
    if (!armed.compare_exchange_strong(operation, None)) return false;
    ++hits;
    return true;
}

template <class Info, class Link>
Link* findLink(const Info* info, VkStructureType type) {
    auto* next = static_cast<const VkBaseInStructure*>(info->pNext);
    for (; next; next = next->pNext) {
        if (next->sType != type) continue;
        auto* link = reinterpret_cast<const Link*>(next);
        if (link->function == VK_LAYER_LINK_INFO) return const_cast<Link*>(link);
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info,
                                              const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* link = findLink<VkInstanceCreateInfo, VkLayerInstanceCreateInfo>(info, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    instanceProc = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    physicalProc = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(instanceProc(nullptr, "vkCreateInstance"));
    VkResult status = create(info, allocator, instance);
    if (status == VK_SUCCESS) currentInstance = *instance;
    return status;
}

VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* link = findLink<VkDeviceCreateInfo, VkLayerDeviceCreateInfo>(info, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    deviceProc = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(next(currentInstance, "vkCreateDevice"));
    VkResult status = create(physical, info, allocator, device);
    if (status == VK_SUCCESS) {
        currentDevice = *device;
        VkPhysicalDeviceProperties properties{};
        auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(next(currentInstance, "vkGetPhysicalDeviceProperties"));
        getProperties(physical, &properties);
        coherentAtomSize = properties.limits.nonCoherentAtomSize;
    }
    return status;
}

template <class F>
F downstream(const char* name) {
    return reinterpret_cast<F>(deviceProc(currentDevice, name));
}

VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice device, VkSemaphore semaphore, uint64_t* value) {
    ++counterReads;
    if (take(Timeline)) return injected;
    if (maskCounter.load()) {
        *value = 0;
        return VK_SUCCESS;
    }
    return downstream<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device, semaphore, value);
}
VKAPI_ATTR VkResult VKAPI_CALL mapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                         VkDeviceSize size, VkMemoryMapFlags flags, void** output) {
    if (auto error = mapError.load(); error != VK_SUCCESS) {
        ++hits;
        return error;
    }
    return downstream<PFN_vkMapMemory>("vkMapMemory")(device, memory, offset, size, flags, output);
}
VKAPI_ATTR void VKAPI_CALL bufferRequirements(VkDevice device, const VkBufferMemoryRequirementsInfo2* info,
                                              VkMemoryRequirements2* requirements) {
    downstream<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2")(device, info, requirements);
    if (!dedicatedBuffers) return;
    // A VMA suballocation could otherwise reuse a block already mapped by the upload pool.
    for (auto* item = static_cast<VkBaseOutStructure*>(requirements->pNext); item; item = item->pNext) {
        if (item->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS)
            reinterpret_cast<VkMemoryDedicatedRequirements*>(item)->requiresDedicatedAllocation = VK_TRUE;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL image(VkDevice device, const VkImageCreateInfo* info,
                                     const VkAllocationCallbacks* allocator, VkImage* output) {
    if (take(Image)) return injected;
    return downstream<PFN_vkCreateImage>("vkCreateImage")(device, info, allocator, output);
}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (take(Submit)) return injected;
    auto next = downstream<PFN_vkQueueSubmit>("vkQueueSubmit");
    if (armGate.exchange(false)) {
        const uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo times{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        times.waitSemaphoreValueCount = 1;
        times.pWaitSemaphoreValues = &value;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO, &times};
        wait.waitSemaphoreCount = 1;
        wait.pWaitSemaphores = &batchGate;
        wait.pWaitDstStageMask = &stage;
        if (auto status = next(queue, 1, &wait, VK_NULL_HANDLE); status != VK_SUCCESS) return status;
    }
    return next(queue, count, submits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR* info) {
    ++presentations;
    // Complete the real presentation and its semaphore wait before substituting WSI status.
    auto status = downstream<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(queue, info);
    if (pauseSite == Present && status == VK_SUCCESS)
        downstream<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue);
    checkpoint(Present);
    return status == VK_SUCCESS && take(Present) ? injected.load() : status;
}
VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice device, VkSwapchainKHR chain, uint64_t timeout,
                                       VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    ++acquisitions;
    checkpoint(Acquire);
    if (armed == Acquire && injected < 0 && take(Acquire)) return injected;
    auto status = downstream<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(device, chain, timeout, semaphore, fence, index);
    return status == VK_SUCCESS && take(Acquire) ? injected.load() : status;
}
VKAPI_ATTR VkResult VKAPI_CALL swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                         const VkAllocationCallbacks* allocator, VkSwapchainKHR* result) {
    if (auto error = createError.load(); error != VK_SUCCESS) {
        ++failedCreates;
        return error;
    }
    return downstream<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(device, info, allocator, result);
}
VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice device) {
    ++idleCalls;
    return downstream<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProc(VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getInstanceProc(VkInstance instance, const char* name);

PFN_vkVoidFunction intercepted(const char* name) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction function;
    };
    const Entry entries[]{
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(createInstance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(createDevice)},
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(getInstanceProc)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(getDeviceProc)},
        {"vkGetSemaphoreCounterValue", reinterpret_cast<PFN_vkVoidFunction>(counter)},
        {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(swapchain)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mapMemory)},
        {"vkGetBufferMemoryRequirements2", reinterpret_cast<PFN_vkVoidFunction>(bufferRequirements)},
        {"vkGetBufferMemoryRequirements2KHR", reinterpret_cast<PFN_vkVoidFunction>(bufferRequirements)},
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(image)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(submit)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(present)},
        {"vkAcquireNextImageKHR", reinterpret_cast<PFN_vkVoidFunction>(acquire)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(idle)}};
    for (const auto& entry : entries)
        if (std::strcmp(name, entry.name) == 0) return entry.function;
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getInstanceProc(VkInstance instance, const char* name) {
    if (auto function = intercepted(name)) return function;
    return instanceProc ? instanceProc(instance, name) : nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProc(VkDevice device, const char* name) {
    if (auto function = intercepted(name)) return function;
    return deviceProc(device, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getPhysicalProc(VkInstance instance, const char* name) {
    return physicalProc ? physicalProc(instance, name) : nullptr;
}
}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (info->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = getInstanceProc;
    info->pfnGetDeviceProcAddr = getDeviceProc;
    info->pfnGetPhysicalDeviceProcAddr = getPhysicalProc;
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) int pauseWorker(int operation) {
    std::unique_lock lock(gateMutex);
    if (operation < 0)
        return gateChanged.wait_for(lock, std::chrono::seconds(5), [] { return atGate; });
    if (operation == None) {
        pauseSite = None;
        gateChanged.notify_all();
        return !gateExpired;
    }
    if (pauseSite || atGate) return 0;
    pauseSite = operation;
    gateExpired = false;
    return 1;
}

extern "C" __declspec(dllexport) void armSurfaceFault(int operation, int result) {
    if (operation == DedicatedBuffers) {
        dedicatedBuffers = result != 0;
        return;
    }
    // VMA can retry a failed mapping with another memory type.
    if (operation == MapMemory) {
        hits = 0;
        mapError = static_cast<VkResult>(result);
        return;
    }
    if (operation == None) mapError = VK_SUCCESS;
    if (operation == MaskTimeline) {
        maskCounter = result != 0;
        return;
    }
    if (operation == CreateSwapchain) {
        if (result) failedCreates = 0;
        createError = static_cast<VkResult>(result);
        return;
    }
    injected = static_cast<VkResult>(result);
    hits = acquisitions = presentations = idleCalls = 0;
    armed = static_cast<Operation>(operation);
}
// Hold the next batch on a real GPU timeline until Java has observed the previous batch.
extern "C" __declspec(dllexport) int gateGpuBatch(int hold) {
    if (hold) {
        if (batchGate) return VK_ERROR_INITIALIZATION_FAILED;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        auto status = downstream<PFN_vkCreateSemaphore>("vkCreateSemaphore")(currentDevice, &info, nullptr, &batchGate);
        armGate = status == VK_SUCCESS;
        return status;
    }
    if (!batchGate) return VK_ERROR_INITIALIZATION_FAILED;
    armGate = false;
    VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    info.semaphore = batchGate;
    info.value = 1;
    auto status = downstream<PFN_vkSignalSemaphore>("vkSignalSemaphore")(currentDevice, &info);
    if (status == VK_SUCCESS) status = downstream<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(currentDevice);
    downstream<PFN_vkDestroySemaphore>("vkDestroySemaphore")(currentDevice, batchGate, nullptr);
    batchGate = VK_NULL_HANDLE;
    return status;
}
extern "C" __declspec(dllexport) uint64_t surfaceTestAtomSize() { return coherentAtomSize; }

extern "C" __declspec(dllexport) int surfaceFaultCounter(int kind) {
    switch (kind) {
        case 0: return hits;
        case 1: return acquisitions;
        case 2: return presentations;
        case 3: return idleCalls;
        case 4: return failedCreates;
        case 5: return counterReads;
        default: return -1;
    }
}
