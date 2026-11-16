package dev.raynium.client.mixin.takeover;

import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.bridge.NativeFrameBuffer;
import dev.raynium.client.takeover.TakeoverGui;
import java.util.Comparator;
import net.minecraft.SharedConstants;
import net.minecraft.client.gui.render.GuiRenderer;
import net.minecraft.client.gui.render.TextureSetup;
import net.minecraft.client.renderer.state.gui.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GuiRenderer.class)
abstract class GuiCaptureMixin implements TakeoverGui.Source {
    @Shadow @Final private GuiRenderState renderState;
    @Shadow @Final private static Comparator<GuiElementRenderState> ELEMENT_SORT_COMPARATOR;
    @Shadow protected abstract void prepareText();
    @Shadow protected abstract void prepareItemElements();
    @Shadow protected abstract void preparePictureInPicture();
    @Shadow protected abstract void clearUnusedOversizedItemRenderers();
    @Unique private TakeoverGui raynium$gui;
    @Unique private NativeFrameBuffer raynium$buffer;
    @Override public void raynium$capture(NativeFrameBuffer buffer) {
        if(renderState.panoramaRenderState!=null) {
            var window=net.minecraft.client.Minecraft.getInstance().gameRenderer.gameRenderState().windowRenderState;
            dev.raynium.client.takeover.scene.ScenePanorama.capture(buffer.scene(),renderState.panoramaRenderState.spin(),window.width,window.height);
        }
        preparePictureInPicture(); prepareItemElements(); prepareText();
        renderState.sortElements(ELEMENT_SORT_COMPARATOR);
        if(raynium$buffer!=buffer) {
            if(raynium$gui!=null) raynium$gui.close();
            raynium$buffer=buffer; raynium$gui=new TakeoverGui(buffer);
        }
        raynium$gui.capture(renderState);
        renderState.reset();
        clearUnusedOversizedItemRenderers();
        if(SharedConstants.DEBUG_SHUFFLE_UI_RENDERING_ORDER) { RenderPipeline.updateSortKeySeed(); TextureSetup.updateSortKeySeed(); }
    }
    @Inject(method="close",at=@At("HEAD"))
    private void raynium$close(CallbackInfo callback) { if(raynium$gui!=null) { raynium$gui.close(); raynium$gui=null; raynium$buffer=null; } }
}
