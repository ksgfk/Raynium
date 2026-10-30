package dev.raynium.bridge;

import dev.raynium.client.preview.VulkanPreviewImage;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.system.MemoryUtil;
import org.lwjgl.vulkan.*;

import static dev.raynium.client.preview.VulkanPreviewImage.check;
import static org.junit.jupiter.api.Assertions.*;
import static org.lwjgl.vulkan.VK12.*;
import static org.lwjgl.vulkan.EXTDebugUtils.*;
import static org.lwjgl.vulkan.EXTValidationFeatures.*;

/** Real independent Vulkan consumer; production importer/copy code is exercised unchanged. */
final class VulkanConsumer implements AutoCloseable {
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    int family;
    long pool, fence, messenger;
    final List<String> errors = new ArrayList<>();
    final VkDebugUtilsMessengerCallbackEXT callback = VkDebugUtilsMessengerCallbackEXT.create((severity, type, data, user) -> {
        String message = VkDebugUtilsMessengerCallbackDataEXT.create(data).pMessageString();
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) errors.add(message);
        System.err.println("Vulkan consumer validation: " + message);
        return VK_FALSE;
    });

    VulkanConsumer() {
        try (MemoryStack stack = MemoryStack.stackPush()) {
            var app = VkApplicationInfo.calloc(stack).sType$Default().apiVersion(VK_API_VERSION_1_2);
            var debug = VkDebugUtilsMessengerCreateInfoEXT.calloc(stack).sType$Default().pfnUserCallback(callback)
                    .messageSeverity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                    .messageType(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
            var validation = VkValidationFeaturesEXT.calloc(stack).sType$Default().pNext(debug.address())
                    .pEnabledValidationFeatures(stack.ints(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT));
            var create = VkInstanceCreateInfo.calloc(stack).sType$Default().pApplicationInfo(app).pNext(validation.address())
                    .ppEnabledExtensionNames(stack.pointers(stack.UTF8("VK_EXT_debug_utils"), stack.UTF8("VK_EXT_validation_features")))
                    .ppEnabledLayerNames(stack.pointers(stack.UTF8("VK_LAYER_KHRONOS_validation")));
            var pointer = stack.mallocPointer(1);
            check(vkCreateInstance(create, null, pointer), "create Vulkan test instance");
            instance = new VkInstance(pointer.get(0), create);
            var handle = stack.mallocLong(1);
            check(vkCreateDebugUtilsMessengerEXT(instance, debug, null, handle), "create validation messenger");
            messenger = handle.get(0);
            var count = stack.mallocInt(1);
            check(vkEnumeratePhysicalDevices(instance, count, null), "enumerate GPUs");
            var physicals = stack.mallocPointer(count.get(0));
            check(vkEnumeratePhysicalDevices(instance, count, physicals), "enumerate GPUs");
            VkPhysicalDevice physical = null;
            for (int i = 0; i < physicals.remaining(); i++) {
                var candidate = new VkPhysicalDevice(physicals.get(i), instance);
                var props = VkPhysicalDeviceProperties.calloc(stack);
                vkGetPhysicalDeviceProperties(candidate, props);
                if (physical == null || props.deviceType() == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) physical = candidate;
                if (props.deviceType() == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
            }
            assertNotNull(physical);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, count, null);
            var queues = VkQueueFamilyProperties.calloc(count.get(0), stack);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, count, queues);
            family = -1;
            for (int i = 0; i < queues.remaining(); i++) if ((queues.get(i).queueFlags() & VK_QUEUE_GRAPHICS_BIT) != 0) { family = i; break; }
            assertTrue(family >= 0);
            var queueInfo = VkDeviceQueueCreateInfo.calloc(1, stack).sType$Default().queueFamilyIndex(family).pQueuePriorities(stack.floats(1));
            var timeline = VkPhysicalDeviceTimelineSemaphoreFeatures.calloc(stack).sType$Default().timelineSemaphore(true);
            var deviceInfo = VkDeviceCreateInfo.calloc(stack).sType$Default().pNext(timeline.address()).pQueueCreateInfos(queueInfo)
                    .ppEnabledExtensionNames(stack.pointers(stack.UTF8("VK_KHR_external_memory_win32"), stack.UTF8("VK_KHR_external_semaphore_win32")));
            check(vkCreateDevice(physical, deviceInfo, null, pointer), "create test device");
            device = new VkDevice(pointer.get(0), physical, deviceInfo);
            vkGetDeviceQueue(device, family, 0, pointer);
            queue = new VkQueue(pointer.get(0), device);
            var poolInfo = VkCommandPoolCreateInfo.calloc(stack).sType$Default().queueFamilyIndex(family);
            check(vkCreateCommandPool(device, poolInfo, null, handle), "create test command pool");
            pool = handle.get(0);
            check(vkCreateFence(device, VkFenceCreateInfo.calloc(stack).sType$Default(), null, handle), "create test fence");
            fence = handle.get(0);
        } catch (RuntimeException | Error failure) {
            close();
            throw failure;
        }
    }

    private int memoryType(int allowed, int required, MemoryStack stack) {
        var properties = VkPhysicalDeviceMemoryProperties.calloc(stack);
        vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), properties);
        for (int i = 0; i < properties.memoryTypeCount(); i++)
            if ((allowed & (1 << i)) != 0 && (properties.memoryTypes(i).propertyFlags() & required) == required) return i;
        throw new IllegalStateException("Test memory type unavailable");
    }

    void copyAndCheck(VulkanPreviewImage shared, long sequence, int width, int height) {
        copyAndCheckSolid(shared,sequence,width,height,null);
    }
    void copyAndCheckSolid(VulkanPreviewImage shared, long sequence, int width, int height, int[] expected) {
        copyAndCheckImage(shared,sequence,width,height,(x,y)->expected);
    }
    void copyAndCheckImage(VulkanPreviewImage shared,long sequence,int width,int height,
                           java.util.function.BiFunction<Integer,Integer,int[]> expectation) {
        int fullWidth = width + 35, fullHeight = height + 29, x0 = 17, y0 = 11;
        int byteCount = fullWidth * fullHeight * 4;
        long target = 0, targetMemory = 0, buffer = 0, bufferMemory = 0;
        try (MemoryStack stack = MemoryStack.stackPush()) {
            var handle = stack.mallocLong(1);
            var imageInfo = VkImageCreateInfo.calloc(stack).sType$Default().imageType(VK_IMAGE_TYPE_2D)
                    .format(VK_FORMAT_R8G8B8A8_UNORM).mipLevels(1).arrayLayers(1).samples(VK_SAMPLE_COUNT_1_BIT)
                    .tiling(VK_IMAGE_TILING_OPTIMAL).usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            imageInfo.extent().set(fullWidth, fullHeight, 1);
            check(vkCreateImage(device, imageInfo, null, handle), "create test main target");
            target = handle.get(0);
            var requirements = VkMemoryRequirements.calloc(stack);
            vkGetImageMemoryRequirements(device, target, requirements);
            var allocation = VkMemoryAllocateInfo.calloc(stack).sType$Default().allocationSize(requirements.size())
                    .memoryTypeIndex(memoryType(requirements.memoryTypeBits(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, stack));
            check(vkAllocateMemory(device, allocation, null, handle), "allocate test target");
            targetMemory = handle.get(0);
            check(vkBindImageMemory(device, target, targetMemory, 0), "bind test target");
            var bufferInfo = VkBufferCreateInfo.calloc(stack).sType$Default().size(byteCount).usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            check(vkCreateBuffer(device, bufferInfo, null, handle), "create test readback");
            buffer = handle.get(0);
            vkGetBufferMemoryRequirements(device, buffer, requirements);
            allocation.allocationSize(requirements.size()).memoryTypeIndex(memoryType(requirements.memoryTypeBits(),
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stack));
            check(vkAllocateMemory(device, allocation, null, handle), "allocate test readback");
            bufferMemory = handle.get(0);
            check(vkBindBufferMemory(device, buffer, bufferMemory, 0), "bind test readback");
            check(vkResetCommandPool(device, pool, 0), "reset test pool");
            check(vkResetFences(device, fence), "reset test fence");
            var commandInfo = VkCommandBufferAllocateInfo.calloc(stack).sType$Default().commandPool(pool).level(VK_COMMAND_BUFFER_LEVEL_PRIMARY).commandBufferCount(1);
            var pointer = stack.mallocPointer(1);
            check(vkAllocateCommandBuffers(device, commandInfo, pointer), "allocate test commands");
            var command = new VkCommandBuffer(pointer.get(0), device);
            check(vkBeginCommandBuffer(command, VkCommandBufferBeginInfo.calloc(stack).sType$Default()), "begin test commands");
            var barrier = VkImageMemoryBarrier.calloc(1, stack).sType$Default().image(target)
                    .oldLayout(VK_IMAGE_LAYOUT_UNDEFINED).newLayout(VK_IMAGE_LAYOUT_GENERAL)
                    .srcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).dstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).dstAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT);
            barrier.subresourceRange().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(1).layerCount(1);
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, null, null, barrier);
            var range = VkImageSubresourceRange.calloc(1, stack).aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(1).layerCount(1);
            var color = VkClearColorValue.calloc(stack).float32(0, 8 / 255f).float32(1, 16 / 255f).float32(2, 32 / 255f).float32(3, 1);
            vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_GENERAL, color, range);
            shared.recordCopy(command, target, x0, y0);
            var copy = VkBufferImageCopy.calloc(1, stack);
            copy.imageSubresource().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).layerCount(1);
            copy.imageExtent().set(fullWidth, fullHeight, 1);
            vkCmdCopyImageToBuffer(command, target, VK_IMAGE_LAYOUT_GENERAL, buffer, copy);
            var hostBarrier = VkMemoryBarrier.calloc(1, stack).sType$Default()
                    .srcAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT).dstAccessMask(VK_ACCESS_HOST_READ_BIT);
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, hostBarrier, null, null);
            check(vkEndCommandBuffer(command), "end test commands");
            var values = VkTimelineSemaphoreSubmitInfo.calloc(stack).sType$Default()
                    .pWaitSemaphoreValues(stack.longs(sequence)).pSignalSemaphoreValues(stack.longs(sequence));
            // LWJGL cannot infer this count from the two parallel wait arrays.
            var submit = VkSubmitInfo.calloc(1, stack).sType$Default().pNext(values.address()).waitSemaphoreCount(1)
                    .pWaitSemaphores(stack.longs(shared.readySemaphore())).pWaitDstStageMask(stack.ints(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))
                    .pCommandBuffers(stack.pointers(command)).pSignalSemaphores(stack.longs(shared.consumedSemaphore()));
            check(vkQueueSubmit(queue, submit, fence), "submit test copy");
            check(vkWaitForFences(device, fence, true, 5_000_000_000L), "wait test copy");
            shared.awaitConsumed(sequence);
            shared.awaitReady(sequence);
            check(vkMapMemory(device, bufferMemory, 0, byteCount, 0, pointer), "map test result");
            try {
                ByteBuffer pixels = MemoryUtil.memByteBuffer(pointer.get(0), byteCount);
                for (int y = 0; y < fullHeight; y++) for (int x = 0; x < fullWidth; x++) {
                    boolean inside = x >= x0 && y >= y0 && x < x0 + width && y < y0 + height;
                    int r = inside ? Math.round((x - x0 + .5f) / width * 255) : 8;
                    int g = inside ? Math.round((1 - (y - y0 + .5f) / height) * 255) : 16;
                    int b = inside ? 0 : 32;
                    int[] expected=inside?expectation.apply(x-x0,y-y0):null;
                    if(expected!=null) { r=expected[0]; g=expected[1]; b=expected[2]; }
                    int offset = (y * fullWidth + x) * 4;
                    assertEquals(r, Byte.toUnsignedInt(pixels.get(offset)), 1, "red x=" + x + " y=" + y + " RGBA="
                        + Byte.toUnsignedInt(pixels.get(offset)) + "," + Byte.toUnsignedInt(pixels.get(offset+1)) + ","
                        + Byte.toUnsignedInt(pixels.get(offset+2)) + "," + Byte.toUnsignedInt(pixels.get(offset+3)));
                    assertEquals(g, Byte.toUnsignedInt(pixels.get(offset + 1)), 1, "green x=" + x + " y=" + y);
                    assertEquals(b, Byte.toUnsignedInt(pixels.get(offset + 2)));
                    assertEquals(255, Byte.toUnsignedInt(pixels.get(offset + 3)));
                }
            } finally { vkUnmapMemory(device, bufferMemory); }
        } finally {
            if (buffer != 0) vkDestroyBuffer(device, buffer, null);
            if (bufferMemory != 0) vkFreeMemory(device, bufferMemory, null);
            if (target != 0) vkDestroyImage(device, target, null);
            if (targetMemory != 0) vkFreeMemory(device, targetMemory, null);
        }
        assertTrue(errors.isEmpty(), String.join("\n", errors));
    }

    void writeTerrainInputs(java.util.List<VulkanPreviewImage> images, boolean split, int width, int height, int mips, long sequence) {
        writeTerrainInputs(images,split,width,height,mips,sequence,false);
    }
    void writeTerrainInputs(java.util.List<VulkanPreviewImage> images, boolean split, int width, int height, int mips, long sequence, boolean flat) {
        long texture=0, memory=0;
        try(var stack=MemoryStack.stackPush()) {
            var handle=stack.mallocLong(1);
            var create=VkImageCreateInfo.calloc(stack).sType$Default().imageType(VK_IMAGE_TYPE_2D).format(VK_FORMAT_R8G8B8A8_UNORM)
                .mipLevels(mips).arrayLayers(1).samples(VK_SAMPLE_COUNT_1_BIT).tiling(VK_IMAGE_TILING_OPTIMAL)
                .usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            create.extent().set(Math.max(width,16),Math.max(height,16),1);
            check(vkCreateImage(device,create,null,handle),"create terrain test source"); texture=handle.get(0);
            var requirements=VkMemoryRequirements.calloc(stack); vkGetImageMemoryRequirements(device,texture,requirements);
            var allocation=VkMemoryAllocateInfo.calloc(stack).sType$Default().allocationSize(requirements.size())
                .memoryTypeIndex(memoryType(requirements.memoryTypeBits(),VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,stack));
            check(vkAllocateMemory(device,allocation,null,handle),"allocate terrain test source"); memory=handle.get(0);
            check(vkBindImageMemory(device,texture,memory,0),"bind terrain test source");
            check(vkResetCommandPool(device,pool,0),"reset producer pool"); check(vkResetFences(device,fence),"reset producer fence");
            var allocate=VkCommandBufferAllocateInfo.calloc(stack).sType$Default().commandPool(pool).level(VK_COMMAND_BUFFER_LEVEL_PRIMARY).commandBufferCount(1);
            var pointer=stack.mallocPointer(1); check(vkAllocateCommandBuffers(device,allocate,pointer),"allocate producer commands");
            var command=new VkCommandBuffer(pointer.get(0),device);
            check(vkBeginCommandBuffer(command,VkCommandBufferBeginInfo.calloc(stack).sType$Default()),"begin producer commands");
            var barrier=VkImageMemoryBarrier.calloc(1,stack).sType$Default().image(texture).oldLayout(VK_IMAGE_LAYOUT_UNDEFINED).newLayout(VK_IMAGE_LAYOUT_GENERAL)
                .srcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).dstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).dstAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT);
            barrier.subresourceRange().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(mips).layerCount(1);
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,null,null,barrier);
            for(int mip=0;mip<mips;mip++) {
                var color=VkClearColorValue.calloc(stack).float32(0,(32+(flat?0:mip)*32)/255f).float32(1,(64+(flat?0:mip)*16)/255f).float32(2,128/255f).float32(3,1);
                var range=VkImageSubresourceRange.calloc(1,stack).aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).baseMipLevel(mip).levelCount(1).layerCount(1);
                vkCmdClearColorImage(command,texture,VK_IMAGE_LAYOUT_GENERAL,color,range);
            }
            for(var image:images) image.beginWrite(command);
            for(int mip=0;mip<mips;mip++) images.get(split?mip:0).copyFrom(command,texture,mip,split?0:mip,0,0,Math.max(1,width>>mip),Math.max(1,height>>mip));
            // Reuse source level zero for a white lightmap, after all atlas reads finish.
            var memoryBarrier=VkMemoryBarrier.calloc(1,stack).sType$Default().srcAccessMask(VK_ACCESS_TRANSFER_READ_BIT).dstAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,memoryBarrier,null,null);
            var white=VkClearColorValue.calloc(stack).float32(0,1).float32(1,1).float32(2,1).float32(3,1);
            var base=VkImageSubresourceRange.calloc(1,stack).aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(1).layerCount(1);
            vkCmdClearColorImage(command,texture,VK_IMAGE_LAYOUT_GENERAL,white,base);
            memoryBarrier.srcAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT).dstAccessMask(VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,memoryBarrier,null,null);
            images.getLast().copyFrom(command,texture,0,0,0,0,16,16);
            for(var image:images) image.endWrite(command);
            check(vkEndCommandBuffer(command),"end producer commands");
            var values=VkTimelineSemaphoreSubmitInfo.calloc(stack).sType$Default().pWaitSemaphoreValues(stack.longs(sequence)).pSignalSemaphoreValues(stack.longs(sequence));
            var submit=VkSubmitInfo.calloc(1,stack).sType$Default().pNext(values.address()).waitSemaphoreCount(1).pCommandBuffers(stack.pointers(command))
                .pWaitSemaphores(stack.longs(images.getFirst().readySemaphore())).pWaitDstStageMask(stack.ints(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))
                .pSignalSemaphores(stack.longs(images.getFirst().consumedSemaphore()));
            check(vkQueueSubmit(queue,submit,fence),"submit terrain inputs"); check(vkWaitForFences(device,fence,true,5_000_000_000L),"wait terrain input copies");
        } finally {
            if(texture!=0) vkDestroyImage(device,texture,null);
            if(memory!=0) vkFreeMemory(device,memory,null);
        }
    }

    @Override public void close() {
        if (device != null) {
            check(vkDeviceWaitIdle(device), "finish test device");
            if (fence != 0) vkDestroyFence(device, fence, null);
            if (pool != 0) vkDestroyCommandPool(device, pool, null);
            vkDestroyDevice(device, null); device = null;
        }
        if (instance != null) {
            if (messenger != 0) vkDestroyDebugUtilsMessengerEXT(instance, messenger, null);
            vkDestroyInstance(instance, null); instance = null;
        }
        callback.free();
        assertTrue(errors.isEmpty(), String.join("\n", errors));
    }
}
