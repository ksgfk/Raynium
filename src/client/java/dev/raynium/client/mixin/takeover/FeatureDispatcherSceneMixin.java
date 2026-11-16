package dev.raynium.client.mixin.takeover;

import dev.raynium.client.takeover.TakeoverRuntime;
import dev.raynium.client.takeover.scene.*;
import java.util.ArrayList;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.Font;
import net.minecraft.client.renderer.StagedVertexBuffer;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.*;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.resources.model.ModelManager;
import net.minecraft.client.resources.model.sprite.AtlasManager;
import org.joml.Matrix4fc;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(FeatureRenderDispatcher.class)
abstract class FeatureDispatcherSceneMixin implements SceneFeatureSource {
    @Shadow @Final private ModelManager modelManager;
    @Shadow @Final private AtlasManager atlasManager;
    @Shadow @Final private Font font;
    @Shadow @Final private GameRenderState gameRenderState;
    @Shadow @Final private StagedVertexBuffer stagedVertexBuffer;
    @Shadow @Final private FeatureRendererMap featureRenderers;
    @Unique private final ArrayList<SceneFeatures> raynium$frames=new ArrayList<>();
    @Unique private final SceneUniforms raynium$uniforms=new SceneUniforms();
    @Override public void raynium$capture(SubmitNodeStorage storage,Matrix4fc projection,Matrix4fc view,boolean oit,int phase,boolean itemsLighting) {
        raynium$captureView(storage,projection,view,oit,phase,itemsLighting,gameRenderState.windowRenderState.width,gameRenderState.windowRenderState.height);
    }
    @Override public void raynium$captureOffscreen(SubmitNodeStorage storage,Matrix4fc projection,Matrix4fc view,int width,int height) {
        raynium$captureView(storage,projection,view,false,dev.raynium.bridge.NativeSceneFrame.OFFSCREEN,false,width,height);
    }
    @Unique private void raynium$captureView(SubmitNodeStorage storage,Matrix4fc projection,Matrix4fc view,boolean oit,int phase,boolean itemsLighting,int width,int height) {
        var client=Minecraft.getInstance(); var output=TakeoverRuntime.frameBuffer().scene();
        SceneFeatures features=null;
        for(var candidate:raynium$frames) if(!candidate.borrowed()) { features=candidate; break; }
        if(features==null) { features=new SceneFeatures(); raynium$frames.add(features); }
        var context=new FeatureFrameContext(gameRenderState.optionsRenderState,font,modelManager.getBlockStateModelSet(),client.getBlockColors(),
            client.getTextureManager(),atlasManager,client.gameRenderer.lightmap(),stagedVertexBuffer);
        var lighting=client.level==null?net.minecraft.world.level.CardinalLighting.Type.DEFAULT:client.level.dimensionType().cardinalLightType();
        var uniforms=raynium$uniforms; uniforms.reset(output,gameRenderState,projection,view,lighting);
        if(itemsLighting) uniforms.itemsLighting();
        if(phase==dev.raynium.bridge.NativeSceneFrame.OFFSCREEN) uniforms.offscreen(SceneOffscreen.lighting());
        features.capture(storage,featureRenderers,context,output,uniforms,width,height,oit,phase);
        TakeoverRuntime.retain(features);
    }
    @Inject(method="close",at=@At("HEAD"))
    private void raynium$close(CallbackInfo ci) { for(var frame:raynium$frames) frame.close(); raynium$frames.clear(); }
}
