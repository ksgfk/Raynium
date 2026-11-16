package dev.raynium.client.mixin;

import dev.raynium.client.validation.VisualValidation;
import net.minecraft.client.particle.Particle;
import net.minecraft.client.particle.ParticleEngine;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ParticleEngine.class)
abstract class VisualValidationParticlesMixin {
    @Inject(method="tick",at=@At("HEAD"),cancellable=true)
    private void raynium$fixedTicks(CallbackInfo callback) {
        if(VisualValidation.controlsParticles() && !VisualValidation.buildingParticles()) callback.cancel();
    }
    @Inject(method="add",at=@At("HEAD"),cancellable=true)
    private void raynium$fixtureOnly(Particle particle,CallbackInfo callback) {
        if(VisualValidation.controlsParticles() && !VisualValidation.buildingParticles()) callback.cancel();
    }
}
