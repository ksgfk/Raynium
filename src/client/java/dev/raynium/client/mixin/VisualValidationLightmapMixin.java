package dev.raynium.client.mixin;

import dev.raynium.client.validation.VisualValidation;
import net.minecraft.client.renderer.LightmapRenderStateExtractor;
import net.minecraft.client.renderer.state.LightmapRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LightmapRenderStateExtractor.class)
abstract class VisualValidationLightmapMixin {
    @Shadow private float blockLightFlicker;
    @Shadow private boolean needsUpdate;
    @Inject(method="extract",at=@At("HEAD"))
    private void raynium$stableLight(LightmapRenderState state,float partialTicks,CallbackInfo callback) {
        if(VisualValidation.controlsScene()) { blockLightFlicker=0; needsUpdate=true; }
    }
}
