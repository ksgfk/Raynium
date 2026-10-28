package dev.raynium.client.mixin;

import com.mojang.renderpearl.backend.vulkan.VulkanCommandEncoder;
import com.mojang.renderpearl.backend.vulkan.VulkanDevice;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(VulkanCommandEncoder.class)
public interface VulkanCommandEncoderAccessor {
    @Accessor("device") VulkanDevice raynium$getDevice();
}
