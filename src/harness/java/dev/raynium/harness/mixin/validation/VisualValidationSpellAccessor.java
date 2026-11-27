package dev.raynium.harness.mixin.validation;

import net.minecraft.client.particle.SpellParticle;
import net.minecraft.util.RandomSource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(SpellParticle.class)
public interface VisualValidationSpellAccessor {
    @Accessor("RANDOM")
    static RandomSource rayniumHarness$random() {
        throw new AssertionError("Mixin accessor");
    }
}
