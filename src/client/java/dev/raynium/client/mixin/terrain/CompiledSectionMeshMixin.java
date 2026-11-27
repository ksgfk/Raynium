package dev.raynium.client.mixin.terrain;

import dev.raynium.client.terrain.PayloadOwner;
import dev.raynium.client.terrain.SectionPayload;
import dev.raynium.client.terrain.TerrainCapture;
import java.util.Map;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.CompiledSectionMesh;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import net.minecraft.client.renderer.chunk.SectionMesh;
import net.minecraft.client.renderer.chunk.TranslucencyPointOfView;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(CompiledSectionMesh.class)
abstract class CompiledSectionMeshMixin implements PayloadOwner {
    @Shadow
    private Map<ChunkSectionLayer, SectionMesh.SectionDraw> draws;
    @Shadow
    public abstract boolean isVertexBufferUploaded(ChunkSectionLayer layer);
    @Shadow
    public abstract boolean isIndexBufferUploaded(ChunkSectionLayer layer);

    @Unique
    private SectionPayload raynium$payload;
    @Unique
    private volatile int raynium$readyMask;

    @Override
    public SectionPayload raynium$payload() {
        return raynium$payload;
    }
    @Override
    public void raynium$payload(SectionPayload payload) {
        raynium$payload = payload;
    }
    @Override
    public int raynium$readyMask() {
        return raynium$readyMask;
    }

    @Unique
    private synchronized void raynium$refreshLayer(ChunkSectionLayer layer) {
        if (this.draws.containsKey(layer) && this.isVertexBufferUploaded(layer) && this.isIndexBufferUploaded(layer)) {
            this.raynium$readyMask |= 1 << (layer.ordinal() * 8);
        }
    }

    @Inject(method = "<init>", at = @At("RETURN"))
    private void raynium$attach(TranslucencyPointOfView pov, SectionCompiler.Results results, long started, CallbackInfo ci) {
        raynium$payload = ((PayloadOwner)(Object)results).raynium$payload();
        ((PayloadOwner)(Object)results).raynium$payload(null);
    }

    @Inject(method = "setVertexBufferUploaded", at = @At("RETURN"))
    private void raynium$onVertexUploaded(ChunkSectionLayer layer, CallbackInfo ci) {
        raynium$refreshLayer(layer);
    }

    @Inject(method = "setIndexBufferUploaded", at = @At("RETURN"))
    private void raynium$onIndexUploaded(ChunkSectionLayer layer, CallbackInfo ci) {
        raynium$refreshLayer(layer);
    }

    @Inject(method = "close", at = @At("HEAD"))
    private void raynium$release(CallbackInfo ci) {
        raynium$readyMask = 0;
        TerrainCapture.removed(raynium$payload);
        raynium$payload = null;
    }
}
