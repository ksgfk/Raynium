package dev.raynium.client.mixin.takeover;
import dev.raynium.client.takeover.TakeoverScreenshots;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.platform.NativeImage;
import java.util.function.Consumer;
import net.minecraft.client.Screenshot;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
@Mixin(Screenshot.class)
abstract class ScreenshotGuardMixin {
    @Inject(method="takeScreenshot(Lcom/mojang/blaze3d/pipeline/RenderTarget;ILjava/util/function/Consumer;)V",at=@At("HEAD"),cancellable=true)
    private static void raynium$capture(RenderTarget target,int scale,Consumer<NativeImage> callback,CallbackInfo ci) {
        TakeoverScreenshots.request(target,scale,callback); ci.cancel();
    }
}
