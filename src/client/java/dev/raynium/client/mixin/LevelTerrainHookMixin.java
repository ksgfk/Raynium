package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.chunk.ChunkSectionsToRender;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.*;

@Mixin(LevelRenderer.class)
abstract class LevelTerrainHookMixin {
    @Inject(method={"prepareChunkRenders","prepareChunkRendersIndirect"},at=@At("RETURN"))
    private void raynium$capture(CallbackInfoReturnable<ChunkSectionsToRender> result) {
        var device=(RayniumGpuDevice)RenderSystem.getDevice();
        ((RetainedTerrain.ViewOwner)result.getReturnValue()).raynium$setView(device.captureTerrain((LevelRenderer)(Object)this));
    }
    @Inject(method={"invalidateCompiledGeometry","resetLevelRenderData"},at=@At("HEAD"))
    private void raynium$invalidate(CallbackInfo ci) { TerrainCapture.invalidate(); }
}
