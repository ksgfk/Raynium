package dev.raynium.client.mixin;

import com.mojang.renderpearl.backend.vulkan.VulkanFeatureSets;
import com.mojang.renderpearl.backend.vulkan.init.FeatureSet;
import java.util.HashSet;
import java.util.Set;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(VulkanFeatureSets.class)
abstract class VulkanFeatureSetsMixin {
    @Inject(method = "optionalFeatureSets", at = @At("RETURN"), cancellable = true)
    private static void raynium$externalResources(CallbackInfoReturnable<Set<FeatureSet>> callback) {
        if (!Boolean.parseBoolean(System.getProperty("raynium.preview.enabled", "true"))) return;
        var features = new HashSet<>(callback.getReturnValue());
        features.add(new FeatureSet("Raynium development preview",
                Set.of("VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"), Set.of()));
        callback.setReturnValue(features);
    }
}
