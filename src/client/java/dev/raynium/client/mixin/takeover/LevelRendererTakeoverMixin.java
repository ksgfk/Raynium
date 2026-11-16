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
    @Shadow @Final private SubmitNodeStorage submitNodeStorage;
    @Shadow @Final private net.minecraft.client.renderer.feature.FeatureRenderDispatcher featureRenderDispatcher;
    @Shadow @Final private GameRenderer gameRenderer;
    @Shadow @Final private WeatherEffectRenderer weatherEffectRenderer;
    @Shadow @Final private WorldBorderRenderer worldBorderRenderer;
    @Shadow @Final private CloudRenderer cloudRenderer;
    @Shadow private SkyRenderer skyRenderer;
    @Shadow @Final private net.minecraft.client.renderer.texture.TextureManager textureManager;
    @Shadow @Final private net.minecraft.client.resources.model.sprite.AtlasManager atlasManager;
    @Shadow public abstract void render(com.mojang.blaze3d.resource.GraphicsResourceAllocator allocator,boolean outline,CameraRenderState camera,
        com.mojang.renderpearl.api.buffers.GpuBufferSlice fog,org.joml.Vector4f color,boolean sky,boolean consistentDepth);
    @Unique private org.joml.Matrix4fc raynium$projection;
    @Shadow protected abstract void submitFeatures(LevelRenderState state,SubmitNodeCollector collector,boolean renderOutline);
    @Shadow protected abstract void repositionCamera(CameraRenderState camera);
    @Shadow protected abstract void compileSections(CameraRenderState camera);
    @Shadow public abstract boolean isSectionCompiledAndVisible(net.minecraft.core.BlockPos pos,long fade);
    @Override public int raynium$transientBlocks() { return transientBlocks.size(); }
    @Override public void raynium$maintain() {
        TakeoverRuntime.maintained();
        var camera=levelRenderState.cameraRenderState;
        repositionCamera(camera);
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
    @Override public void raynium$capture(org.joml.Matrix4fc projection,boolean renderOutline) {
        if(raynium$projection!=null) throw new IllegalStateException("Recursive world feature capture");
        raynium$projection=projection;
        try { render(null,renderOutline,levelRenderState.cameraRenderState,null,levelRenderState.cameraRenderState.fogData.color,false,false); }
        finally { raynium$projection=null; }
    }
    // Keep method-entry lifecycle hooks, including Fabric's render context initialization.
    @Inject(method="render",at=@At(value="INVOKE",target="Lnet/minecraft/util/profiling/Profiler;get()Lnet/minecraft/util/profiling/ProfilerFiller;"),cancellable=true)
    private void raynium$features(com.mojang.blaze3d.resource.GraphicsResourceAllocator allocator,boolean renderOutline,CameraRenderState camera,
        com.mojang.renderpearl.api.buffers.GpuBufferSlice fog,org.joml.Vector4f color,boolean sky,boolean consistentDepth,CallbackInfo ci) {
        if(raynium$projection==null) throw new IllegalStateException("World rendering requires a native scene view");
        boolean oit=gameRenderer.useImprovedTransparency();
        submitNodeStorage.setUseImprovedTransparency(oit);
        var stack=com.mojang.blaze3d.systems.RenderSystem.getModelViewStack(); stack.pushMatrix();
        com.mojang.blaze3d.systems.RenderSystem.isRenderingLevel=true;
        try {
            stack.mul(levelRenderState.cameraRenderState.viewRotationMatrix);
            var fogType=camera.fogType;
            if(fogType!=net.minecraft.world.level.material.FogType.POWDER_SNOW && fogType!=net.minecraft.world.level.material.FogType.LAVA
                && !camera.entityRenderState.doesMobEffectBlockSky) {
                if(levelRenderState.shouldResetSkyRenderer || skyRenderer==null) {
                    if(skyRenderer!=null) skyRenderer.close();
                    skyRenderer=new SkyRenderer(textureManager,atlasManager,gameRenderer.mainRenderTarget());
                }
                ((dev.raynium.client.takeover.scene.SceneSky.Source)skyRenderer).raynium$capture(gameRenderer.gameRenderState(),raynium$projection,stack);
            }
            submitFeatures(levelRenderState,submitNodeStorage,renderOutline);
            ((dev.raynium.client.takeover.scene.SceneFeatureSource)featureRenderDispatcher).raynium$capture(submitNodeStorage,raynium$projection,stack,oit);
            ((dev.raynium.client.takeover.scene.SceneEnvironmentSource)cloudRenderer).raynium$capture(gameRenderer.gameRenderState(),raynium$projection,stack,oit);
            var border=(dev.raynium.client.takeover.scene.SceneEnvironmentSource)worldBorderRenderer;
            var weather=(dev.raynium.client.takeover.scene.SceneEnvironmentSource)weatherEffectRenderer;
            if(oit) border.raynium$capture(gameRenderer.gameRenderState(),raynium$projection,stack,true);
            weather.raynium$capture(gameRenderer.gameRenderState(),raynium$projection,stack,oit);
            if(!oit) border.raynium$capture(gameRenderer.gameRenderState(),raynium$projection,stack,false);
        } finally { stack.popMatrix(); com.mojang.blaze3d.systems.RenderSystem.isRenderingLevel=false; }
        ci.cancel();
    }
    @Inject(method={"invalidateCompiledGeometry","resetLevelRenderData"},at=@At("HEAD"))
    private void raynium$invalidate(CallbackInfo ci) { TerrainCapture.invalidate(); }
}
