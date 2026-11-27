package dev.raynium.client.mixin.backend;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.mojang.renderpearl.api.device.BackendCreationException;
import com.mojang.renderpearl.api.device.GpuBackend;
import dev.raynium.client.renderpearl.RayniumGpuBackend;
import net.minecraft.client.Minecraft;
import net.minecraft.client.PreferredGraphicsApi;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(Minecraft.class)
abstract class RayniumBackendMixin {
    @WrapOperation(method = "<init>", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/PreferredGraphicsApi;getBackendsToTry()[Lcom/mojang/renderpearl/api/device/GpuBackend;"))
    private GpuBackend[] raynium$backends(PreferredGraphicsApi preference, Operation<GpuBackend[]> original) {
        return new GpuBackend[] {new RayniumGpuBackend()};
    }
    @WrapOperation(method = "<init>", at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/backend/vulkan/VulkanBackend;checkBackendAvailable()Lcom/mojang/renderpearl/api/device/BackendCreationException;"))
    private BackendCreationException raynium$skipVanillaProbe(Operation<BackendCreationException> original) {
        return null;
    }
}
