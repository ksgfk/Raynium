package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.client.takeover.TakeoverHandles;
import dev.raynium.client.takeover.scene.SceneOffscreen;
import net.minecraft.client.gui.render.pip.PictureInPictureRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import net.minecraft.client.renderer.state.gui.GuiRenderState;
import net.minecraft.client.renderer.state.gui.pip.PictureInPictureRenderState;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(PictureInPictureRenderer.class)
abstract class PictureInPictureSceneMixin<T extends PictureInPictureRenderState> {
    @Shadow private GpuTexture texture;
    @Shadow private GpuTextureView textureView;
    @Shadow @Final private SubmitNodeStorage submitNodeStorage;
    @Shadow protected abstract boolean textureIsReadyToBlit(T state);
    @Shadow protected abstract float getTranslateY(int height,int scale);
    @Shadow protected abstract void renderToTexture(T state,PoseStack pose,SubmitNodeCollector collector);
    @Shadow protected abstract void blitTexture(T state,GuiRenderState gui);
    @Inject(method="prepare",at=@At("HEAD"),cancellable=true)
    private void raynium$prepare(T state,GuiRenderState gui,FeatureRenderDispatcher dispatcher,int scale,CallbackInfo ci) {
        int width=Math.multiplyExact(state.x1()-state.x0(),scale),height=Math.multiplyExact(state.y1()-state.y0(),scale);
        if(width<=0 || height<=0) throw new IllegalArgumentException("Invalid picture-in-picture extent");
        boolean resize=texture==null || texture.getWidth(0)!=width || texture.getHeight(0)!=height;
        if(resize || !textureIsReadyToBlit(state)) {
            if(resize) {
                if(textureView!=null) textureView.close(); if(texture!=null) texture.close();
                texture=TakeoverHandles.texture("Picture in picture",13,GpuFormat.RGBA8_UNORM,width,height,1,1);
                textureView=TakeoverHandles.view(texture,0,1);
            }
            var stack=RenderSystem.getModelViewStack(); stack.pushMatrix();
            try {
                var pose=new PoseStack(); pose.translate(width/2f,getTranslateY(height,scale),0);
                float factor=scale*state.scale(); pose.scale(factor,factor,-factor);
                renderToTexture(state,pose,submitNodeStorage);
                SceneOffscreen.capture(texture,0,0,width,height,submitNodeStorage,dispatcher,stack);
            } finally { stack.popMatrix(); }
        }
        blitTexture(state,gui); ci.cancel();
    }
}
