package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.terrain.*;
import java.util.EnumMap;
import java.util.Map;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.ChunkSectionsToRender;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.*;

@Mixin(LevelRenderer.class)
abstract class LevelTerrainHookMixin {
    @Inject(method="prepareChunkRenders",at=@At("HEAD"),cancellable=true)
    private void raynium$captureSeparate(CallbackInfoReturnable<ChunkSectionsToRender> cir) {
        if(!TerrainCapture.active()) return;
        var device=(RayniumGpuDevice)RenderSystem.getDevice();
        ChunkSectionsToRender placeholder=new ChunkSectionsToRender.DrawSeparate(null,Map.of(),0,null);
        ((RetainedTerrain.ViewOwner)placeholder).raynium$setView(device.captureTerrain((LevelRenderer)(Object)this));
        cir.setReturnValue(placeholder);
    }

    @Inject(method="prepareChunkRendersIndirect",at=@At("HEAD"),cancellable=true)
    private void raynium$captureIndirect(CallbackInfoReturnable<ChunkSectionsToRender> cir) {
        if(!TerrainCapture.active()) return;
        var device=(RayniumGpuDevice)RenderSystem.getDevice();
        ChunkSectionsToRender placeholder=new ChunkSectionsToRender.DrawIndirect(null,new EnumMap<>(ChunkSectionLayer.class),0,null);
        ((RetainedTerrain.ViewOwner)placeholder).raynium$setView(device.captureTerrain((LevelRenderer)(Object)this));
        cir.setReturnValue(placeholder);
    }

    @Inject(method="clearVisibleSections",at=@At("HEAD"))
    private void raynium$clearVisible(CallbackInfo ci) { TerrainCapture.bumpVisibleListVersion(); }

    @Inject(method={"invalidateCompiledGeometry","resetLevelRenderData"},at=@At("HEAD"))
    private void raynium$invalidate(CallbackInfo ci) {
        if(TerrainCapture.active()) ((RayniumGpuDevice)RenderSystem.getDevice()).retireTerrain();
    }
}
