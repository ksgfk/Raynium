package dev.raynium.client.preview;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.backend.vulkan.VulkanCommandEncoder;
import com.mojang.renderpearl.backend.vulkan.VulkanDevice;
import com.mojang.renderpearl.backend.vulkan.VulkanGpuTexture;
import com.mojang.renderpearl.frontend.FrontendCommandEncoder;
import dev.raynium.bridge.NativePreview;
import dev.raynium.bridge.NativeRuntime;
import dev.raynium.client.mixin.VulkanCommandEncoderAccessor;
import net.minecraft.client.Minecraft;
import org.slf4j.Logger;

import static org.lwjgl.vulkan.VK12.*;

// See doc/architecture/minecraft-integration.md for the temporary preview lifecycle.
public final class PreviewOverlay implements AutoCloseable {
    public record Region(int x, int y, int width, int height) {
        public static Region of(int width, int height) {
            if (width <= 0 || height <= 0) throw new IllegalArgumentException("Invalid main target dimensions");
            int w = Math.max(1, width / 4), h = Math.max(1, height / 4);
            // MC 26.3 flips the main image vertically in VulkanGpuSurface.blitFromTexture.
            return new Region(width - w - Math.min(16, width - w), height - h - Math.min(16, height - h), w, h);
        }
    }

    private final NativeRuntime runtime;
    private final Logger logger;
    private final VulkanCommandEncoder encoder;
    private final VulkanDevice device;
    private NativePreview preview;
    private VulkanPreviewImage imported;
    private int width, height;
    private long drawn, copied;
    private boolean disabled;
    private dev.raynium.client.terrain.TerrainPreview terrain;

    private static VulkanCommandEncoder encoder() {
        var command = RenderSystem.getDevice().createCommandEncoder();
        return command instanceof FrontendCommandEncoder frontend && frontend.backend() instanceof VulkanCommandEncoder vulkan ? vulkan : null;
    }

    public static NativeRuntime.HostGpu hostGpu(Logger logger) {
        var encoder = encoder();
        if (encoder == null) return null;
        try {
            var device = ((VulkanCommandEncoderAccessor) encoder).raynium$getDevice();
            return VulkanPreviewImage.hostGpu(device.vkDevice().getPhysicalDevice());
        } catch (RuntimeException failure) {
            logger.warn("Cannot select preview host GPU: {}", failure.getMessage());
            return null;
        }
    }

    public PreviewOverlay(NativeRuntime runtime, Logger logger) {
        this.runtime = runtime; this.logger = logger;
        encoder = encoder();
        device = encoder == null ? null : ((VulkanCommandEncoderAccessor) encoder).raynium$getDevice();
        disabled = encoder == null || !Boolean.parseBoolean(System.getProperty("raynium.preview.enabled", "true"));
        dev.raynium.client.terrain.TerrainCapture.activate(!disabled);
        if (disabled) logger.info("Raynium preview disabled: {}", encoder == null ? "Minecraft is not using Vulkan" : "raynium.preview.enabled=false");
    }

    public void prepareTerrain(Minecraft client) {
        if(disabled || !dev.raynium.client.terrain.TerrainCapture.ENABLED || client.getWindow().isIconified()) return;
        try {
            if(terrain==null) terrain=new dev.raynium.client.terrain.TerrainPreview(runtime,device,encoder,logger);
            terrain.prepare(client);
        } catch(RuntimeException failure) { disable(failure); }
    }
    private void disable(RuntimeException failure) {
        closeOutput();
        if(terrain!=null) { terrain.close(); terrain=null; }
        disabled=true;
        dev.raynium.client.terrain.TerrainCapture.activate(false);
        logger.warn("Raynium preview disabled for this session; vanilla rendering continues",failure);
    }

    public void render(Minecraft client) {
        if (disabled || client.getWindow().isIconified()) return;
        if(dev.raynium.client.terrain.TerrainCapture.ENABLED && client.level==null && terrain!=null) {
            terrain.close(); terrain=null; return;
        }
        if(dev.raynium.client.terrain.TerrainCapture.ENABLED && (terrain==null || !terrain.pending())) return;
        var target = client.gameRenderer.mainRenderTarget();
        if (target.width <= 0 || target.height <= 0) return;
        try {
            var region = Region.of(target.width, target.height);
            if (preview == null || width != region.width() || height != region.height()) {
                closeOutput();
                preview = runtime.createPreview(region.width(), region.height());
                imported = VulkanPreviewImage.create(device.vkDevice(), device.graphicsQueue().queueFamilyIndex(), preview.info());
                width = region.width(); height = region.height();
                logger.info("Raynium shared preview ready: {}x{} handleType={}", width, height, preview.info().handleType());
            }
            if (!(target.getColorTexture() instanceof VulkanGpuTexture color))
                throw new IllegalStateException("Main target is not a Vulkan texture");
            if(dev.raynium.client.terrain.TerrainCapture.ENABLED) {
                if(!terrain.draw(preview,drawn+1)) throw new IllegalStateException("Terrain input prepared without a matching world frame");
            } else preview.draw(drawn + 1);
            drawn++;
            encoder.waitSemaphore(imported.readySemaphore(), drawn, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            var command = encoder.allocateAndBeginTransientCommandBuffer();
            imported.recordCopy(command, color.vkImage(), region.x(), region.y());
            VulkanPreviewImage.check(vkEndCommandBuffer(command), "end preview copy command buffer");
            encoder.execute(command);
            encoder.signalSemaphore(imported.consumedSemaphore(), drawn, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            copied = drawn;
        } catch (RuntimeException failure) {
            // Cleanup may itself fail on a GPU timeout; then propagate and retain live owners.
            disable(failure);
        }
    }

    private void closeOutput() {
        if (preview == null) return;
        if (imported != null) {
            // Include partially recorded waits/copies if this is failure cleanup.
            try (var completion = encoder.createFence()) {
                encoder.submit();
                if (!completion.awaitCompletion(5_000_000_000L))
                    throw new IllegalStateException("MC preview submission timed out; resources remain live");
            }
            imported.awaitConsumed(copied);
            imported.awaitReady(drawn);
            imported.close();
            imported = null;
        }
        preview.close();
        preview = null;
        drawn = copied = 0;
    }

    @Override public void close() {
        closeOutput();
        if(terrain!=null) { terrain.close(); terrain=null; }
        disabled = true;
        dev.raynium.client.terrain.TerrainCapture.activate(false);
    }
}
