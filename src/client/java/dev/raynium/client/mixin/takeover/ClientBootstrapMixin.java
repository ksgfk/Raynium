package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import net.minecraft.client.ClientBootstrap;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(ClientBootstrap.class)
abstract class ClientBootstrapMixin {
    @WrapOperation(method="bootstrap",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/backend/vulkan/VulkanFeatureSets;bootstrap()V"))
    private static void raynium$skipFeatures(Operation<Void> original) {}
}
