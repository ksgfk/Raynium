package dev.raynium.client.mixin;
import dev.raynium.client.terrain.TerrainCapture;
import dev.raynium.client.terrain.TerrainReference;
import net.minecraft.client.renderer.chunk.ChunkSectionsToRender;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.state.OptionsRenderState;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.*;

@Mixin(LevelRenderer.class)
abstract class LevelTerrainMixin {
    @Shadow private GpuSampler chunkLayerSampler;
    @Shadow @Final private LevelRenderState levelRenderState;
    @Shadow @Final private OptionsRenderState optionsRenderState;
    @Inject(method={"invalidateCompiledGeometry","resetLevelRenderData"},at=@At("HEAD"))
    private void raynium$invalidate(CallbackInfo ci) { TerrainCapture.invalidate(); }
    @Inject(method={"prepareChunkRenders","prepareChunkRendersIndirect"},at=@At("RETURN"))
    private void raynium$referenceDraws(CallbackInfoReturnable<ChunkSectionsToRender> ci) { TerrainReference.prepared(ci.getReturnValue()); }
    @Inject(method="executeOutline",at=@At("HEAD"))
    private void raynium$reference(CallbackInfo ci) { TerrainReference.render(net.minecraft.client.Minecraft.getInstance(),chunkLayerSampler); }
    @WrapOperation(method="extractSectionDrawGroups",at=@At(value="INVOKE",
        target="Lnet/minecraft/client/renderer/chunk/SectionRenderDispatcher;unlock()V"))
    private void raynium$freeze(SectionRenderDispatcher dispatcher,Operation<Void> original,boolean respectTranslucentOrder) {
        try { TerrainCapture.capture((LevelRenderer)(Object)this,levelRenderState.cameraRenderState,optionsRenderState,!respectTranslucentOrder); }
        finally { original.call(dispatcher); }
    }
}
