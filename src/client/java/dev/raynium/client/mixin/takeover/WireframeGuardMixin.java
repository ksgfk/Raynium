package dev.raynium.client.mixin.takeover;
import com.mojang.blaze3d.systems.RenderSystem;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;
@Mixin(RenderSystem.class)
abstract class WireframeGuardMixin {
    @Inject(method="isWireframeAvailable",at=@At("HEAD"),cancellable=true)
    private static void raynium$unsupported(CallbackInfoReturnable<Boolean> ci) { ci.setReturnValue(false); }
}
