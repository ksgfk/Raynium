package dev.raynium.harness.mixin.validation;

import net.minecraft.world.level.block.entity.TheEndGatewayBlockEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(TheEndGatewayBlockEntity.class)
public interface VisualValidationGatewayAccessor {
    @Accessor("age")
    void rayniumHarness$age(long value);
    @Accessor("teleportCooldown")
    void rayniumHarness$cooldown(int value);
}
