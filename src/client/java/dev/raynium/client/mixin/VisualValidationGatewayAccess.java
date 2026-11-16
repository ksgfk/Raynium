package dev.raynium.client.mixin;

import net.minecraft.world.level.block.entity.TheEndGatewayBlockEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(TheEndGatewayBlockEntity.class)
public interface VisualValidationGatewayAccess {
    @Accessor("age") void raynium$age(long value);
    @Accessor("teleportCooldown") void raynium$cooldown(int value);
}
