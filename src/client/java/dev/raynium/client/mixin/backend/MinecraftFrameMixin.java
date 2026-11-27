package dev.raynium.client.mixin.backend;

import dev.raynium.client.terrain.TerrainCapture;
import dev.raynium.diagnostics.ClientFrameEvent;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
abstract class MinecraftFrameMixin {
    @Unique
    private ClientFrameEvent raynium$frameEvent;

    @Inject(method = "renderFrame", at = @At("HEAD"))
    private void raynium$beginFrame(CallbackInfo callback) {
        var client = (Minecraft)(Object)this;
        if (TerrainCapture.active()) {
            TerrainCapture.beginFrame();
        }
        raynium$frameEvent = ClientFrameEvent.start(client.level != null);
    }

    @Inject(method = "renderFrame", at = @At("RETURN"))
    private void raynium$endFrame(CallbackInfo callback) {
        if (raynium$frameEvent != null) {
            raynium$frameEvent.commit();
            raynium$frameEvent = null;
        }
    }
}
