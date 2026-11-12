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
    @Shadow @Final private GuiRenderer guiRenderer;
    @Shadow @Final private com.mojang.blaze3d.resource.CrossFrameResourcePool resourcePool;
    @Shadow protected abstract void bobHurt(CameraRenderState camera,PoseStack stack);
    @Shadow protected abstract void bobView(CameraRenderState camera,PoseStack stack);
    @Inject(method="preloadUiShader",at=@At("HEAD"),cancellable=true)
    private static void raynium$skipUiShader(CallbackInfo ci) { ci.cancel(); }
    @Override public void raynium$prepare() {
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
                image.upload(0,0,0,16,16,CpuLightmap.pixels(state.lightmapRenderState));
            }
            TakeoverRuntime.prepareTerrain(client);
        }
        ((TakeoverGui.Source)guiRenderer).raynium$capture(TakeoverRuntime.frameBuffer());
        guiRenderer.endFrame();
        resourcePool.endFrame();
    }
}
