package dev.raynium.client.mixin.takeover;

import dev.raynium.client.takeover.*;
import it.unimi.dsi.fastutil.longs.Long2ObjectLinkedOpenHashMap;
import net.minecraft.client.renderer.*;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import net.minecraft.client.renderer.state.OptionsRenderState;
import net.minecraft.client.renderer.state.level.*;
import net.minecraft.util.Util;
import org.spongepowered.asm.mixin.*;
import dev.raynium.client.terrain.TerrainCapture;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LevelRenderer.class)
abstract class LevelRendererTakeoverMixin implements LevelMaintenance {
    @Shadow @Final private LevelRenderState levelRenderState;
    @Shadow @Final private OptionsRenderState optionsRenderState;
    @Shadow @Final private SectionOcclusionGraph sectionOcclusionGraph;
    @Shadow @Final private Long2ObjectLinkedOpenHashMap<TransientBlockRenderState> transientBlocks;
    @Shadow private SectionRenderDispatcher sectionRenderDispatcher;
    @Shadow protected abstract void repositionCamera(CameraRenderState camera);
    @Shadow protected abstract void finalizeGizmoCollection();
    @Shadow protected abstract void performTransientBlockRemovals();
    @Shadow protected abstract void compileSections(CameraRenderState camera);
    @Shadow public abstract boolean isSectionCompiledAndVisible(net.minecraft.core.BlockPos pos,long fade);
    @Override public int raynium$transientBlocks() { return transientBlocks.size(); }
    @Override public void raynium$maintain() {
        TakeoverRuntime.maintained();
        var camera=levelRenderState.cameraRenderState;
        repositionCamera(camera);
        finalizeGizmoCollection();
        performTransientBlockRemovals();
        long now=Util.getMillis();
        var iterator=transientBlocks.long2ObjectEntrySet().fastIterator();
        while(iterator.hasNext()) if(now>=iterator.next().getValue().liveUntilMs) iterator.remove();
        levelRenderState.entityRenderStates.clear();
        levelRenderState.blockEntityRenderStates.clear();
        levelRenderState.blockBreakingRenderStates.clear();
        compileSections(camera);
        if(sectionRenderDispatcher!=null) {
            sectionRenderDispatcher.lock();
            try { sectionRenderDispatcher.uploadTerrainBuffersToGpu(); }
            finally { sectionRenderDispatcher.unlock(); }
        }
        sectionOcclusionGraph.update(camera,optionsRenderState.fov,levelRenderState.chunkLoadingRenderState);
        var callback=levelRenderState.playerCompiledSectionCallback;
        if(callback!=null && isSectionCompiledAndVisible(camera.blockPos,Util.toMillis(optionsRenderState.chunkSectionFadeInTime))) callback.run();
    }
    @Inject(method={"invalidateCompiledGeometry","resetLevelRenderData"},at=@At("HEAD"))
    private void raynium$invalidate(CallbackInfo ci) { TerrainCapture.invalidate(); }
}
