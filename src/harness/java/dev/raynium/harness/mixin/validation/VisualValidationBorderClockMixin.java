package dev.raynium.harness.mixin.validation;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.renderer.WorldBorderRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(WorldBorderRenderer.class)
abstract class VisualValidationBorderClockMixin {
    @WrapOperation(method = "prepareDynamicTransforms", at = @At(value = "INVOKE", target = "Lnet/minecraft/util/Util;getMillis()J"))
    private long rayniumHarness$animationTime(Operation<Long> original) {
        return VisualValidation.animationMillis(original.call());
    }
}
