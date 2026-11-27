package dev.raynium.harness.mixin.validation;

import com.mojang.blaze3d.platform.Window;
import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.MouseHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(MouseHandler.class)
abstract class VisualValidationMouseMixin {
    @Inject(method = "getScaledXPos(Lcom/mojang/blaze3d/platform/Window;)D", at = @At("HEAD"), cancellable = true)
    private void rayniumHarness$mouseX(Window window, CallbackInfoReturnable<Double> ci) {
        if (VisualValidation.controlsScene())
            ci.setReturnValue(window.getGuiScaledWidth() / 2.0);
    }
    @Inject(method = "getScaledYPos(Lcom/mojang/blaze3d/platform/Window;)D", at = @At("HEAD"), cancellable = true)
    private void rayniumHarness$mouseY(Window window, CallbackInfoReturnable<Double> ci) {
        if (VisualValidation.controlsScene())
            ci.setReturnValue(window.getGuiScaledHeight() / 2.0);
    }
}
