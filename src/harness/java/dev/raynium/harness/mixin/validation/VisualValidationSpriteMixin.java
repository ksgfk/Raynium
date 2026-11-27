package dev.raynium.harness.mixin.validation;

import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.renderer.texture.SpriteContents;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(SpriteContents.AnimationState.class)
abstract class VisualValidationSpriteMixin {
    @Shadow
    private int frame;
    @Shadow
    private int subFrame;
    @Shadow
    private boolean isDirty;
    @Shadow
    @Final
    private SpriteContents.AnimatedTexture animationInfo;

    @Inject(method = "tick", at = @At("HEAD"), cancellable = true)
    private void rayniumHarness$fixedTextureTime(CallbackInfo callback) {
        long phase = VisualValidation.spritePhase(animationInfo.frames);
        if (phase < 0)
            return;
        int next = (int)(phase >>> 32);
        isDirty |= animationInfo.frames.get(frame).index() != animationInfo.frames.get(next).index();
        frame = next;
        subFrame = (int)phase;
        callback.cancel();
    }
}
