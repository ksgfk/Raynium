package dev.raynium.client.takeover;

import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import net.minecraft.client.renderer.RenderPipelines;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class TakeoverGuiTest {
    @Test void sameIdentifierDoesNotPermitDifferentPipelineState() {
        assertEquals(0,TakeoverGui.pipelineKind(RenderPipelines.GUI));
        assertEquals(12,TakeoverGui.pipelineKind(RenderPipelines.END_PORTAL));
        assertEquals(12,TakeoverGui.pipelineKind(RenderPipelines.END_GATEWAY));
        var imitation=RenderPipeline.builder(RenderPipelines.GUI_SNIPPET).withLocation("pipeline/gui").withCull(true).build();
        assertEquals(RenderPipelines.GUI.getLocation(),imitation.getLocation());
        assertThrows(IllegalArgumentException.class,()->TakeoverGui.pipelineKind(imitation));
        assertThrows(IllegalArgumentException.class,()->TakeoverGui.pipelineKind(RenderPipelines.ITEM_CUTOUT));
    }
}
