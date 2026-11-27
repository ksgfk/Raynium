package dev.raynium.harness.mixin.validation;

import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
abstract class VisualValidationMinecraftMixin {
    @Inject(method = "renderFrame", at = @At("HEAD"))
    private void rayniumHarness$beforeValidationFrame(CallbackInfo callback) {
        VisualValidation.beforeFrame((Minecraft)(Object)this);
    }

    @Inject(method = "renderFrame", at = @At("RETURN"))
    private void rayniumHarness$afterValidationFrame(CallbackInfo callback) {
        VisualValidation.afterFrame((Minecraft)(Object)this);
    }
}
