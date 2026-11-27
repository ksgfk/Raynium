package dev.raynium.harness.mixin.validation;

import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(LivingEntity.class)
public interface VisualValidationUseItemAccessor {
    @Accessor("useItemRemaining")
    void rayniumHarness$useItemRemaining(int value);
}
