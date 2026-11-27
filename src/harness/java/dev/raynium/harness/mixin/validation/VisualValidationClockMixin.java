package dev.raynium.harness.mixin.validation;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.renderer.rendertype.TextureTransform;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(TextureTransform.class)
abstract class VisualValidationClockMixin {
    @WrapOperation(method = "setupGlintTexturing", at = @At(value = "INVOKE", target = "Lnet/minecraft/util/Util;getMillis()J"))
    private static long rayniumHarness$animationTime(Operation<Long> original) {
        return VisualValidation.animationMillis(original.call());
    }
}
