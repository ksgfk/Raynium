package dev.raynium.harness.mixin.validation;

import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class VisualValidationPostMixin {
    @Inject(method = "update", at = @At("RETURN"))
    private void rayniumHarness$requestedEffect(CallbackInfo callback) {
        var effect = VisualValidation.postEffect();
        if (effect != null)
            ((GameRenderer)(Object)this).getRequestedPostEffects().add(effect);
    }
}
