package dev.raynium.client.mixin;

import dev.raynium.client.validation.VisualValidation;
import net.minecraft.client.DeltaTracker;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(DeltaTracker.Timer.class)
abstract class VisualValidationPartialTickMixin {
    @Inject(method="getGameTimeDeltaPartialTick",at=@At("RETURN"),cancellable=true)
    private void raynium$fixedPartialTick(boolean ignoreFrozenGame,CallbackInfoReturnable<Float> callback) {
        callback.setReturnValue(VisualValidation.partialTick(callback.getReturnValue()));
    }
}
