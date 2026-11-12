package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.pipeline.MainTarget;
import com.mojang.blaze3d.pipeline.RenderTarget;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(MainTarget.class)
abstract class MainTargetMixin extends RenderTarget {
    private MainTargetMixin() { super("Raynium",null,null); }
    @Inject(method="createFrameBuffer",at=@At("HEAD"),cancellable=true)
    private void raynium$size(int width,int height,CallbackInfo ci) { this.width=width; this.height=height; ci.cancel(); }
}
