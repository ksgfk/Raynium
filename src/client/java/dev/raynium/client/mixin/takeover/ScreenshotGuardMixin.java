package dev.raynium.client.mixin.takeover;
import dev.raynium.client.takeover.UnsupportedCapture;
import net.minecraft.client.Screenshot;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
@Mixin(Screenshot.class)
abstract class ScreenshotGuardMixin {
    @Inject(method="grab(Lnet/minecraft/client/Minecraft;Z)V",at=@At("HEAD"),cancellable=true)
    private static void raynium$unsupported(CallbackInfo ci) { UnsupportedCapture.warn(); ci.cancel(); }
}
