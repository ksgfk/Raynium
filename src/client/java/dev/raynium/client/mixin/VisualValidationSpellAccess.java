package dev.raynium.client.mixin;

import net.minecraft.client.particle.SpellParticle;
import net.minecraft.util.RandomSource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(SpellParticle.class)
public interface VisualValidationSpellAccess {
    @Accessor("RANDOM") static RandomSource raynium$random() { throw new AssertionError("Mixin accessor"); }
}
