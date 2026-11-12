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

@Mixin(GuiRenderer.class)
abstract class GuiCaptureMixin implements TakeoverGui.Source {
    @Shadow @Final private GuiRenderState renderState;
    @Shadow @Final private static Comparator<GuiElementRenderState> ELEMENT_SORT_COMPARATOR;
    @Shadow protected abstract void prepareText();
    @Shadow protected abstract void clearUnusedOversizedItemRenderers();
    @Override public void raynium$capture(NativeFrameBuffer buffer) {
        prepareText();
        renderState.sortElements(ELEMENT_SORT_COMPARATOR);
        new TakeoverGui(buffer).capture(renderState);
        renderState.reset();
        clearUnusedOversizedItemRenderers();
        if(SharedConstants.DEBUG_SHUFFLE_UI_RENDERING_ORDER) { RenderPipeline.updateSortKeySeed(); TextureSetup.updateSortKeySeed(); }
    }
}
