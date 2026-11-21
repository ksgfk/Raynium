package dev.raynium.client.terrain;

import dev.raynium.bridge.*;
import dev.raynium.client.renderpearl.CommandTape;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.texture.TextureAtlas;

public final class RetainedTerrain {
    private final TerrainResidency residency=new TerrainResidency();
    private final NativeRenderer.Frame frameScratch=new NativeRenderer.Frame();
    private long captureEpoch;
    private long renderEpoch;

    public CommandTape.TerrainView capture(LevelRenderer level,NativeRenderer renderer,CommandTape batch) {
        var event=batch.measureTerrain();
        long start=event==null?0:System.nanoTime();
        var client=Minecraft.getInstance();
        var game=client.gameRenderer;
        var state=game.gameRenderState();
        TerrainCapture.capture(level,state.levelRenderState.cameraRenderState,state.optionsRenderState,game.useImprovedTransparency(),renderEpoch);
        long captured=event==null?0:System.nanoTime();
        var snapshot=TerrainCapture.snapshot();
        if(snapshot==null) throw new IllegalStateException("Terrain preparation requires a current projection");
        if(captureEpoch!=snapshot.epoch) {
            captureEpoch=snapshot.epoch;
            renderEpoch=renderer.nextEpoch();
            residency.reset(renderEpoch);
        }
        var atlas=(NativeTexture)client.getTextureManager().getTexture(TextureAtlas.LOCATION_BLOCKS).getTexture();
        var light=(NativeTexture)game.lightmap().texture();
        var storage=batch.acquireViewStorage();
        CommandTape.TerrainView view;
        try {
            TerrainCapture.drainRemoved(residency.pendingRemovals);
            boolean allowReuse=batch.viewCount()==0;
            residency.prepare(snapshot,storage,atlas.id(),light.id(),allowReuse);
            var f=frameScratch;
            f.epoch=renderEpoch; f.outputSequence=NativeRenderer.nextFrameId();
            f.flags=(snapshot.rgss?NativeRenderer.RGSS:0)|(snapshot.improvedTransparency?NativeRenderer.IMPROVED_TRANSPARENCY:0)|
                (state.levelRenderState.renderWireframeTerrain?NativeRenderer.WIREFRAME:0)|residency.reuseFlags();
            f.projection=snapshot.projection; f.view=snapshot.view; f.cameraBlock=snapshot.block; f.cameraFraction=snapshot.fraction;
            f.fogColor=snapshot.fog; f.fogDistances=snapshot.distances;
            view=batch.addView(storage,storage.finish(f),atlas,light);
        } catch(RuntimeException | Error failure) { storage.close(); throw failure; }
        var onSubmit=residency.stageCommit();
        if(onSubmit!=TerrainResidency.NOOP) batch.onSubmitted(onSubmit);
        if(event!=null) {
            event.inputPreparation+=captured-start;
            event.preparation+=System.nanoTime()-captured;
            event.visible+=residency.effectiveVisibleCount();
        }
        return view;
    }
    public interface ViewOwner {
        void raynium$setView(CommandTape.TerrainView view);
    }
}
