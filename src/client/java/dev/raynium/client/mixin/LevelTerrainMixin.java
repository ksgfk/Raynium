package dev.raynium.client.mixin;
import dev.raynium.client.terrain.TerrainCapture;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.state.OptionsRenderState;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.*;

@Mixin(LevelRenderer.class)
abstract class LevelTerrainMixin {
    @Shadow @Final private LevelRenderState levelRenderState;
    @Shadow @Final private OptionsRenderState optionsRenderState;
    @Inject(method={"invalidateCompiledGeometry","resetLevelRenderData"},at=@At("HEAD"))
    private void raynium$invalidate(CallbackInfo ci) { TerrainCapture.invalidate(); }
    @Inject(method={"prepareChunkRenders","prepareChunkRendersIndirect"},at=@At("HEAD"))
    private void raynium$freeze(CallbackInfoReturnable<?> ci) {
        TerrainCapture.capture((LevelRenderer)(Object)this,levelRenderState.cameraRenderState,optionsRenderState);
    }
}
