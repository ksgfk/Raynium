package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import net.minecraft.client.renderer.RenderPipelines;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class VanillaPipelineCatalogTest {
    @Test void sameLocationItemVariantsHaveDifferentProgramIdentities() {
        var catalog=VanillaPipelineCatalog.create();
        var plain=catalog.require(RenderPipelines.ITEM_CUTOUT);
        var glint=catalog.require(RenderPipelines.ITEM_CUTOUT_GLINT);
        var special=catalog.require(RenderPipelines.ITEM_CUTOUT_GLINT_SPECIAL);
        assertEquals(plain.pipeline().getLocation(),glint.pipeline().getLocation());
        assertEquals(plain.pipeline().getLocation(),special.pipeline().getLocation());
        assertNotEquals(plain.id(),glint.id()); assertNotEquals(glint.id(),special.id());
        assertNotEquals(plain.identity(),glint.identity()); assertNotEquals(glint.identity(),special.identity());
    }
    @Test void everyRegisteredPipelineAndOitFieldHasStableCatalogEntry() {
        var first=VanillaPipelineCatalog.create(); var second=VanillaPipelineCatalog.create();
        for(var pipeline:RenderPipelines.requiredPipelines()) assertFalse(first.require(pipeline).optional());
        for(var pipeline:RenderPipelines.optionalPipelines()) assertTrue(first.require(pipeline).optional());
        assertTrue(first.entries().stream().anyMatch(entry->entry.names().stream().anyMatch(name->name.contains("/ACCUMULATE"))));
        assertEquals(first.entries(),second.entries());
    }
    @Test void unknownPipelineCannotAliasAnExistingName() {
        var catalog=VanillaPipelineCatalog.create();
        var imitation=RenderPipeline.builder(RenderPipelines.GUI_SNIPPET).withLocation("pipeline/gui").build();
        assertThrows(IllegalArgumentException.class,()->catalog.require(imitation));
    }
}
