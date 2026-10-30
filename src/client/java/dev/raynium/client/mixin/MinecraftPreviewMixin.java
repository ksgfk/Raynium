package dev.raynium.client.mixin;

import dev.raynium.client.preview.PreviewHost;
import dev.raynium.client.preview.PreviewOverlay;
import dev.raynium.client.preview.ClientFrameEvent;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
abstract class MinecraftPreviewMixin implements PreviewHost {
    @Unique private PreviewOverlay raynium$preview;
    @Unique private ClientFrameEvent raynium$frameEvent;

    @Override public void raynium$setPreview(PreviewOverlay preview) { raynium$preview = preview; }

    @Override public void raynium$prepareTerrain() { if(raynium$preview!=null) raynium$preview.prepareTerrain((Minecraft)(Object)this); }
    @Inject(method="renderFrame",at=@At("HEAD"))
    private void raynium$beginFrame(CallbackInfo ci) {
        raynium$frameEvent = ClientFrameEvent.start(((Minecraft)(Object)this).level != null);
        dev.raynium.client.terrain.TerrainCapture.beginFrame();
    }

    @Inject(method="renderFrame",at=@At("RETURN"))
    private void raynium$endFrame(CallbackInfo ci) {
        if (raynium$frameEvent != null) {
            raynium$frameEvent.end();
            raynium$frameEvent.commit();
            raynium$frameEvent = null;
        }
    }

    @Inject(method = "renderFrame", at = @At(value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER))
    private void raynium$copyPreview(CallbackInfo callback) {
        if (raynium$preview != null) raynium$preview.render((Minecraft) (Object) this);
    }
}
