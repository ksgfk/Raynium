package dev.raynium.client.mixin;

import dev.raynium.client.world.ParticleFastTrack;
import java.util.Map;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.state.level.QuadParticleRenderState;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;

@Mixin(QuadParticleRenderState.class)
abstract class QuadParticleRenderStateMixin implements ParticleFastTrack.RawParticleAccess {
    @Shadow @Final private Map<SingleQuadParticle.Layer, QuadParticleRenderState.Storage> particles;

    @Override
    public boolean raynium$hasCompactSlice(SingleQuadParticle.Layer layer) {
        var storage = particles.get(layer);
        return storage != null && storage.currentParticleIndex > 0;
    }

    @Override
    public int raynium$appendCompactLayer(SingleQuadParticle.Layer layer, ParticleFastTrack.CompactLayerBatch out) {
        QuadParticleRenderState.Storage storage = this.particles.get(layer);
        if (storage == null || storage.currentParticleIndex <= 0) return 0;
        out.append(storage.floatValues, storage.intValues, storage.currentParticleIndex);
        return storage.currentParticleIndex;
    }
}
