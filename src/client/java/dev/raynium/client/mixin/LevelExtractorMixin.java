package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.terrain.TerrainCapture;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.extract.LevelExtractor;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LevelExtractor.class)
abstract class LevelExtractorMixin {
    @Inject(method="setLevel",at=@At("HEAD"))
    private void raynium$retireWorld(ClientLevel level,CallbackInfo ci) {
        if(level==null && TerrainCapture.active()) ((RayniumGpuDevice)RenderSystem.getDevice()).retireTerrain();
    }
}
