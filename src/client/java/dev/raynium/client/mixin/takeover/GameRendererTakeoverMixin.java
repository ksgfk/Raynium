package dev.raynium.client.mixin.takeover;

import dev.raynium.client.takeover.*;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.gui.render.GuiRenderer;
import dev.raynium.client.terrain.TerrainCapture;
import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Matrix4f;
import org.joml.Vector3f;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class GameRendererTakeoverMixin implements TakeoverRuntime.FrameSource {
    @Unique private final byte[] raynium$lightmapPixels=new byte[16*16*4];
    @Shadow @Final private GuiRenderer guiRenderer;
    @Shadow private boolean useUiLightmap;
    @Shadow @Final private com.mojang.blaze3d.resource.CrossFrameResourcePool resourcePool;
    @Shadow @Final private net.minecraft.client.renderer.SubmitNodeStorage handAndScreenSubmitNodeStorage;
    @Shadow @Final private net.minecraft.client.renderer.ScreenEffectRenderer screenEffectRenderer;
    @Shadow @Final private net.minecraft.client.renderer.DebugCrosshairRenderer debugCrosshairRenderer;
    @Shadow protected abstract void bobHurt(CameraRenderState camera,PoseStack stack);
    @Shadow protected abstract void bobView(CameraRenderState camera,PoseStack stack);
    @Shadow protected abstract boolean shouldRenderBlockOutline();
    @Shadow protected abstract void tryTakeScreenshotIfNeeded();
    @Inject(method="preloadUiShader",at=@At("HEAD"),cancellable=true)
    private static void raynium$skipUiShader(CallbackInfo ci) { ci.cancel(); }
    @Override public void raynium$prepare() { raynium$prepare(false); }
    @Override public void raynium$prepareWorldOnly() { raynium$prepare(true); }
    @Unique private void raynium$prepare(boolean worldOnly) {
        GameRenderer game=(GameRenderer)(Object)this;
        var state=game.gameRenderState();
        var window=state.windowRenderState;
        if(game.mainRenderTarget().width!=window.width || game.mainRenderTarget().height!=window.height) game.resize(window.width,window.height);
        TakeoverRuntime.beginFrame(state);
        if(state.shouldRenderLevel) {
            var client=Minecraft.getInstance();
            ((LevelMaintenance)client.levelRenderer).raynium$maintain();
            var camera=state.levelRenderState.cameraRenderState;
            var projection=new Matrix4f(camera.projectionMatrix);
            var bob=new PoseStack();
            bobHurt(camera,bob);
            if(state.optionsRenderState.bobView) bobView(camera,bob);
            projection.mul(bob.last().pose());
            var player=state.levelRenderState.playerRenderState;
            float effect=Math.max(player.portalEffectIntensity,player.nauseaEffectIntensity)*state.optionsRenderState.screenEffectScale*state.optionsRenderState.screenEffectScale;
            if(effect>0) {
                float skew=5/(effect*effect+5)-effect*.04f; skew*=skew;
                var axis=new Vector3f(0,(float)Math.sqrt(2)/2,(float)Math.sqrt(2)/2);
                float angle=player.spinningEffectAngle*(float)(Math.PI/180);
                projection.rotate(angle,axis).scale(1/skew,1,1).rotate(-angle,axis);
            }
            TerrainCapture.projection(projection);
            TerrainCapture.capture(client.levelRenderer,camera,state.optionsRenderState,game.useImprovedTransparency());
            if(state.lightmapRenderState.needsUpdate) {
                var image=((TakeoverHandles.Texture)game.levelLightmap().texture()).nativeTexture();
                CpuLightmap.write(state.lightmapRenderState,raynium$lightmapPixels);
                image.upload(0,0,0,16,16,raynium$lightmapPixels);
            }
            TakeoverRuntime.prepareTerrain(client);
            ((LevelMaintenance)client.levelRenderer).raynium$capture(projection,shouldRenderBlockOutline());
            raynium$hands(game);
            if(!worldOnly) {
                TakeoverScreenshots.atWorldStage(this::tryTakeScreenshotIfNeeded);
                dev.raynium.client.takeover.scene.ScenePost.world(TakeoverRuntime.frameBuffer().scene(),state);
            }
        }
        if(worldOnly) return;
        game.lighting().setupFor(com.mojang.blaze3d.platform.Lighting.Entry.ITEMS_3D);
        useUiLightmap=true;
        try {
            ((TakeoverGui.Source)guiRenderer).raynium$capture(TakeoverRuntime.frameBuffer());
            guiRenderer.endFrame();
        } finally { useUiLightmap=false; }
        game.renderBuffers().endFrame();
        resourcePool.endFrame();
    }
    @Unique private void raynium$hands(GameRenderer game) {
        var state=game.gameRenderState(); var camera=state.levelRenderState.cameraRenderState;
        var player=state.levelRenderState.playerRenderState; var client=Minecraft.getInstance();
        var projection=new Matrix4f().setPerspective((float)Math.toRadians(camera.hudFov),
            (float)state.windowRenderState.width/state.windowRenderState.height,camera.depthFar,.05f,true);
        var source=(dev.raynium.client.takeover.scene.SceneFeatureSource)game.featureRenderDispatcher();
        var stack=com.mojang.blaze3d.systems.RenderSystem.getModelViewStack();
        handAndScreenSubmitNodeStorage.setUseImprovedTransparency(false);
        if(!camera.isPanoramicMode && player.hasPlayer && state.optionsRenderState.cameraType.isFirstPerson()
            && !camera.entityRenderState.isSleeping && !state.guiRenderState.isHudHidden
            && client.gameMode.getPlayerMode()!=net.minecraft.world.level.GameType.SPECTATOR) {
            var pose=new PoseStack(); pose.mulPose(camera.viewRotationMatrix.invert(new Matrix4f()));
            stack.pushMatrix().mul(camera.viewRotationMatrix);
            try {
                bobHurt(camera,pose); if(state.optionsRenderState.bobView) bobView(camera,pose);
                if(player.firstPersonHandsAndItems!=null) game.firstPersonHandsAndItemsRenderer.submitHandsWithItems(
                    camera.cameraEntityPartialTicks,pose,handAndScreenSubmitNodeStorage,player,player.firstPersonHandsAndItems);
                source.raynium$capture(handAndScreenSubmitNodeStorage,projection,stack,false,dev.raynium.bridge.NativeSceneFrame.HAND,false);
            } finally { stack.popMatrix(); }
        }
        screenEffectRenderer.submit(state.levelRenderState.worldPartialTicks,handAndScreenSubmitNodeStorage,player,camera,state.guiRenderState.isHudHidden);
        source.raynium$capture(handAndScreenSubmitNodeStorage,projection,stack,false,dev.raynium.bridge.NativeSceneFrame.HAND,player.itemActivation!=null);
        if(state.levelRenderState.render3dCrosshair && state.optionsRenderState.cameraType.isFirstPerson() && !state.guiRenderState.isHudHidden)
            ((dev.raynium.client.takeover.scene.SceneEnvironmentSource)debugCrosshairRenderer).raynium$capture(state,projection,stack,false);
    }
}
