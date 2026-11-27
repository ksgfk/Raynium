package dev.raynium.client.mixin.terrain;

import dev.raynium.client.terrain.TerrainCapture;
import net.minecraft.client.renderer.SectionOcclusionGraph;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(SectionOcclusionGraph.class)
abstract class SectionOcclusionGraphMixin {
    @Inject(method = "addSectionsInFrustum", at = @At("RETURN"))
    private void raynium$onVisibleSectionsUpdated(CallbackInfo ci) {
        TerrainCapture.bumpVisibleListVersion();
    }
}
