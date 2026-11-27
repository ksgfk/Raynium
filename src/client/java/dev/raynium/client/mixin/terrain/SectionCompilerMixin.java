package dev.raynium.client.mixin.terrain;
import dev.raynium.client.terrain.MeshPayload;
import dev.raynium.client.terrain.PayloadOwner;
import dev.raynium.client.terrain.SectionPayload;
import dev.raynium.client.terrain.TerrainCapture;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(SectionCompiler.class)
abstract class SectionCompilerMixin {
    @Unique
    private long raynium$epoch;
    @Inject(method = "<init>", at = @At("RETURN"))
    private void raynium$generation(CallbackInfo ci) {
        raynium$epoch = TerrainCapture.epoch();
    }
    @Inject(method = "compile", at = @At("RETURN"))
    private void raynium$capture(CallbackInfoReturnable<SectionCompiler.Results> cir) {
        if (!TerrainCapture.active())
            return;
        var result = cir.getReturnValue();
        var layers = new MeshPayload[3];
        for (var layer : ChunkSectionLayer.values()) {
            var mesh = result.renderedLayers.get(layer);
            if (mesh != null)
                layers[layer.ordinal()] = new MeshPayload(raynium$epoch, mesh.vertexBuffer(), layer.ordinal());
        }
        ((PayloadOwner)(Object)result).raynium$payload(new SectionPayload(raynium$epoch, layers));
    }
}
