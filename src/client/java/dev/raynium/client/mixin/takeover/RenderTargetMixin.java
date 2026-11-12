package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import dev.raynium.client.takeover.TakeoverRuntime;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(RenderTarget.class)
abstract class RenderTargetMixin {
    @Shadow public int width;
    @Shadow public int height;
    @Inject(method="createBuffers",at=@At("HEAD"),cancellable=true)
    private void raynium$size(int width,int height,CallbackInfo ci) {
        RenderSystem.assertOnRenderThread();
        int maximum=TakeoverRuntime.deviceInfo().limits().maxTextureSize();
        if(width<=0 || height<=0 || width>maximum || height>maximum) throw new IllegalArgumentException("Invalid takeover target extent");
        this.width=width; this.height=height; ci.cancel();
    }
}
