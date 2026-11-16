package dev.raynium.client.mixin;

import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(LivingEntity.class)
public interface VisualValidationUseItemAccess {
    @Accessor("useItemRemaining") void raynium$useItemRemaining(int value);
}
