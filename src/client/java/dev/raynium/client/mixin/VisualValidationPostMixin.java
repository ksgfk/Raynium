package dev.raynium.client.mixin;

import dev.raynium.client.validation.VisualValidation;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class VisualValidationPostMixin {
    @Inject(method="update",at=@At("RETURN"))
    private void raynium$requestedEffect(CallbackInfo callback) {
        var effect=VisualValidation.postEffect();
        if(effect!=null) ((GameRenderer)(Object)this).getRequestedPostEffects().add(effect);
    }
}
