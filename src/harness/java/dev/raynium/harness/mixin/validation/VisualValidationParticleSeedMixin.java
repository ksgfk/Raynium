package dev.raynium.harness.mixin.validation;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.raynium.harness.validation.VisualValidation;
import net.minecraft.client.particle.Particle;
import net.minecraft.util.RandomSource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(Particle.class)
abstract class VisualValidationParticleSeedMixin {
    @WrapOperation(method = "<init>", at = @At(value = "INVOKE", target = "Lnet/minecraft/util/RandomSource;create()Lnet/minecraft/util/RandomSource;"))
    private RandomSource rayniumHarness$fixtureSeed(Operation<RandomSource> original) {
        return VisualValidation.buildingParticles() ? RandomSource.create(VisualValidation.nextParticleSeed()) : original.call();
    }
}
