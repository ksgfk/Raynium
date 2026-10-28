package dev.raynium.client.preview;

import dev.raynium.bridge.NativePreview;
import dev.raynium.bridge.NativeRuntime;
import java.nio.ByteBuffer;
import java.util.Arrays;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.system.JNI;
import org.lwjgl.vulkan.*;

import static org.lwjgl.vulkan.VK12.*;
import static org.lwjgl.vulkan.KHRExternalMemoryWin32.*;
import static org.lwjgl.vulkan.KHRExternalSemaphoreWin32.*;

/** Owns only importing-device objects. The caller completes GPU use before close. */
public final class VulkanPreviewImage implements AutoCloseable {
    private final VkDevice device;
    private final int queueFamily;
    private final NativePreview.Info info;
    private final long image, memory, ready, consumed;
    private boolean closed;

    public static NativeRuntime.HostGpu hostGpu(VkPhysicalDevice physical) {
        try (MemoryStack stack = MemoryStack.stackPush()) {
            var id = identity(physical, stack);
            if (!id.deviceLUIDValid() || id.deviceNodeMask() != 1)
                throw new IllegalStateException("Vulkan host has no single-node Windows GPU identity");
            return new NativeRuntime.HostGpu(bytes(id.deviceLUID()), id.deviceNodeMask());
        }
    }

    private static VkPhysicalDeviceIDProperties identity(VkPhysicalDevice physical, MemoryStack stack) {
        var id = VkPhysicalDeviceIDProperties.calloc(stack).sType$Default();
        var properties = VkPhysicalDeviceProperties2.calloc(stack).sType$Default().pNext(id.address());
        vkGetPhysicalDeviceProperties2(physical, properties);
        return id;
    }

    private static byte[] bytes(ByteBuffer buffer) {
        byte[] data = new byte[buffer.remaining()];
        buffer.get(buffer.position(), data);
        return data;
    }

    public static VulkanPreviewImage create(VkDevice device, int queueFamily, NativePreview.Info info) {
        if (!device.getCapabilities().VK_KHR_external_memory_win32 || !device.getCapabilities().VK_KHR_external_semaphore_win32)
            throw new IllegalStateException("MC Vulkan external memory/semaphore Win32 extensions are unavailable");
        boolean opaque = info.handleType() == 2;
        if (info.handleType() != 1 && !opaque) throw new IllegalArgumentException("Unknown shared handle type");
        if (info.width() <= 0 || info.height() <= 0 || !info.dedicated() || info.allocationOffset() != 0
                || info.memoryHandle() == 0 || info.readyHandle() == 0 || info.consumedHandle() == 0)
            throw new IllegalArgumentException("Invalid shared preview descriptor");
        int memoryType = opaque ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        int semaphoreType = opaque ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        int usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        long image = 0, memory = 0, ready = 0, consumed = 0;
        boolean success = false;
        try (MemoryStack stack = MemoryStack.stackPush()) {
            var id = identity(device.getPhysicalDevice(), stack);
            if (!id.deviceLUIDValid() || id.deviceNodeMask() != info.nodeMask() || !Arrays.equals(bytes(id.deviceLUID()), info.luid()))
                throw new IllegalStateException("Runtime and MC GPU identities differ");
            if (opaque && (!Arrays.equals(bytes(id.deviceUUID()), info.deviceUuid()) || !Arrays.equals(bytes(id.driverUUID()), info.driverUuid())))
                throw new IllegalStateException("Runtime and MC Vulkan device/driver UUIDs differ");
            if (opaque && (info.nativeFormat() != VK_FORMAT_R8G8B8A8_UNORM || info.nativeUsage() != usage
                    || info.nativeFlags() != 0 || info.nativeTiling() != VK_IMAGE_TILING_OPTIMAL))
                throw new IllegalArgumentException("Opaque image creation parameters differ");
            var externalFormat = VkPhysicalDeviceExternalImageFormatInfo.calloc(stack).sType$Default().handleType(memoryType);
            var formatQuery = VkPhysicalDeviceImageFormatInfo2.calloc(stack).sType$Default().pNext(externalFormat.address())
                    .format(VK_FORMAT_R8G8B8A8_UNORM).type(VK_IMAGE_TYPE_2D).tiling(VK_IMAGE_TILING_OPTIMAL).usage(usage);
            var externalProperties = VkExternalImageFormatProperties.calloc(stack).sType$Default();
            var formatProperties = VkImageFormatProperties2.calloc(stack).sType$Default().pNext(externalProperties.address());
            check(vkGetPhysicalDeviceImageFormatProperties2(device.getPhysicalDevice(), formatQuery, formatProperties), "query shared image support");
            if ((externalProperties.externalMemoryProperties().externalMemoryFeatures() & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0)
                throw new IllegalStateException("MC GPU cannot import this shared image type");
            var timeline = VkSemaphoreTypeCreateInfo.calloc(stack).sType$Default().semaphoreType(VK_SEMAPHORE_TYPE_TIMELINE);
            var fenceQuery = VkPhysicalDeviceExternalSemaphoreInfo.calloc(stack).sType$Default().pNext(timeline.address()).handleType(semaphoreType);
            var fenceSupport = VkExternalSemaphoreProperties.calloc(stack).sType$Default();
            vkGetPhysicalDeviceExternalSemaphoreProperties(device.getPhysicalDevice(), fenceQuery, fenceSupport);
            if ((fenceSupport.externalSemaphoreFeatures() & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0)
                throw new IllegalStateException("MC GPU cannot import this timeline semaphore type");

            var externalImage = VkExternalMemoryImageCreateInfo.calloc(stack).sType$Default().handleTypes(memoryType);
            var imageInfo = VkImageCreateInfo.calloc(stack).sType$Default().pNext(externalImage.address())
                    .imageType(VK_IMAGE_TYPE_2D).format(VK_FORMAT_R8G8B8A8_UNORM).mipLevels(1).arrayLayers(1)
                    .samples(VK_SAMPLE_COUNT_1_BIT).tiling(VK_IMAGE_TILING_OPTIMAL).usage(usage)
                    .sharingMode(VK_SHARING_MODE_EXCLUSIVE).initialLayout(VK_IMAGE_LAYOUT_UNDEFINED);
            imageInfo.extent().set(info.width(), info.height(), 1);
            var handle = stack.mallocLong(1);
            check(vkCreateImage(device, imageInfo, null, handle), "create imported image");
            image = handle.get(0);
            var requirements = VkMemoryRequirements.calloc(stack);
            vkGetImageMemoryRequirements(device, image, requirements);
            var properties = VkPhysicalDeviceMemoryProperties.calloc(stack);
            vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), properties);
            int selected;
            long size;
            if (opaque) {
                selected = info.memoryTypeIndex();
                size = info.allocationSize();
                if (selected < 0 || selected >= properties.memoryTypeCount()
                        || (requirements.memoryTypeBits() & (1 << selected)) == 0
                        || (properties.memoryTypes(selected).propertyFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0
                        || size < requirements.size())
                    throw new IllegalStateException("Opaque allocation size or memory type is incompatible");
            } else {
                var handleProperties = VkMemoryWin32HandlePropertiesKHR.calloc(stack).sType$Default();
                check(vkGetMemoryWin32HandlePropertiesKHR(device, memoryType, info.memoryHandle(), handleProperties), "query D3D12 memory handle");
                selected = memoryType(properties, handleProperties.memoryTypeBits() & requirements.memoryTypeBits());
                size = requirements.size();
            }
            var dedicated = VkMemoryDedicatedAllocateInfo.calloc(stack).sType$Default().image(image);
            var imported = VkImportMemoryWin32HandleInfoKHR.calloc(stack).sType$Default().pNext(dedicated.address())
                    .handleType(memoryType).handle(info.memoryHandle());
            var allocation = VkMemoryAllocateInfo.calloc(stack).sType$Default().pNext(imported.address())
                    .allocationSize(size).memoryTypeIndex(selected);
            check(vkAllocateMemory(device, allocation, null, handle), "import shared memory");
            memory = handle.get(0);
            check(vkBindImageMemory(device, image, memory, 0), "bind shared memory");
            ready = importSemaphore(device, info.readyHandle(), semaphoreType, stack);
            consumed = importSemaphore(device, info.consumedHandle(), semaphoreType, stack);
            var result = new VulkanPreviewImage(device, queueFamily, info, image, memory, ready, consumed);
            success = true;
            return result;
        } finally {
            if (!success) destroy(device, image, memory, ready, consumed);
        }
    }

    private VulkanPreviewImage(VkDevice device, int queueFamily, NativePreview.Info info,
                               long image, long memory, long ready, long consumed) {
        this.device = device; this.queueFamily = queueFamily; this.info = info;
        this.image = image; this.memory = memory; this.ready = ready; this.consumed = consumed;
    }

    private static int memoryType(VkPhysicalDeviceMemoryProperties properties, int allowed) {
        for (int i = 0; i < properties.memoryTypeCount(); i++)
            if ((allowed & (1 << i)) != 0 && (properties.memoryTypes(i).propertyFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) return i;
        throw new IllegalStateException("No compatible device-local import memory type");
    }

    private static long importSemaphore(VkDevice device, long handle, int type, MemoryStack stack) {
        var timeline = VkSemaphoreTypeCreateInfo.calloc(stack).sType$Default().semaphoreType(VK_SEMAPHORE_TYPE_TIMELINE);
        var create = VkSemaphoreCreateInfo.calloc(stack).sType$Default().pNext(timeline.address());
        var output = stack.mallocLong(1);
        check(vkCreateSemaphore(device, create, null, output), "create importing semaphore");
        long semaphore = output.get(0);
        boolean success = false;
        try {
            var imported = VkImportSemaphoreWin32HandleInfoKHR.calloc(stack).sType$Default().semaphore(semaphore).handleType(type).handle(handle);
            // LWJGL 3.4.3 incorrectly requires both handle and name. Vulkan requires
            // name=NULL for a handle import; keep that contract and use its checked entry point address.
            long function = device.getCapabilities().vkImportSemaphoreWin32HandleKHR;
            if (function == 0 || handle == 0) throw new IllegalStateException("Missing semaphore import function or handle");
            check(JNI.callPPI(device.address(), imported.address(), function), "import shared timeline semaphore");
            success = true;
            return semaphore;
        } finally {
            if (!success) vkDestroySemaphore(device, semaphore, null);
        }
    }

    public long readySemaphore() { return ready; }
    public long consumedSemaphore() { return consumed; }

    /** Records acquire/copy/release. Caller queues a ready wait and a consumed signal around it. */
    public void recordCopy(VkCommandBuffer command, long destination, int x, int y) {
        if (closed) throw new IllegalStateException("Imported preview is closed");
        try (MemoryStack stack = MemoryStack.stackPush()) {
            memoryBarrier(command, stack);
            ownershipBarrier(command, stack, true);
            var region = VkImageCopy.calloc(1, stack);
            region.srcSubresource().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).layerCount(1);
            region.dstSubresource().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).layerCount(1);
            region.dstOffset().set(x, y, 0);
            region.extent().set(info.width(), info.height(), 1);
            vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL, region);
            ownershipBarrier(command, stack, false);
            memoryBarrier(command, stack);
        }
    }

    private void ownershipBarrier(VkCommandBuffer command, MemoryStack stack, boolean acquire) {
        var barrier = VkImageMemoryBarrier.calloc(1, stack).sType$Default().image(image)
                .oldLayout(VK_IMAGE_LAYOUT_GENERAL).newLayout(VK_IMAGE_LAYOUT_GENERAL)
                .srcQueueFamilyIndex(acquire ? VK_QUEUE_FAMILY_EXTERNAL : queueFamily)
                .dstQueueFamilyIndex(acquire ? queueFamily : VK_QUEUE_FAMILY_EXTERNAL)
                .srcAccessMask(acquire ? 0 : VK_ACCESS_TRANSFER_READ_BIT)
                .dstAccessMask(acquire ? VK_ACCESS_TRANSFER_READ_BIT : 0);
        barrier.subresourceRange().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(1).layerCount(1);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, null, null, barrier);
    }

    private static void memoryBarrier(VkCommandBuffer command, MemoryStack stack) {
        var barrier = VkMemoryBarrier.calloc(1, stack).sType$Default()
                .srcAccessMask(VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                .dstAccessMask(VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier, null, null);
    }

    public void awaitReady(long value) { await(ready, value); }
    public void awaitConsumed(long value) { await(consumed, value); }

    private void await(long semaphore, long value) {
        if (value == 0) return;
        try (MemoryStack stack = MemoryStack.stackPush()) {
            var wait = VkSemaphoreWaitInfo.calloc(stack).sType$Default().semaphoreCount(1)
                    .pSemaphores(stack.longs(semaphore)).pValues(stack.longs(value));
            check(vkWaitSemaphores(device, wait, 5_000_000_000L), "wait preview completion (timeout is not completion)");
        }
    }

    public static void check(int result, String operation) {
        if (result != VK_SUCCESS) throw new IllegalStateException(operation + " failed: VkResult=" + result);
    }

    private static void destroy(VkDevice device, long image, long memory, long ready, long consumed) {
        if (consumed != 0) vkDestroySemaphore(device, consumed, null);
        if (ready != 0) vkDestroySemaphore(device, ready, null);
        if (image != 0) vkDestroyImage(device, image, null);
        if (memory != 0) vkFreeMemory(device, memory, null);
    }

    @Override public void close() {
        if (!closed) {
            destroy(device, image, memory, ready, consumed);
            closed = true;
        }
    }
}
