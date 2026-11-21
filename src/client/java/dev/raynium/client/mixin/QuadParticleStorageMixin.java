package dev.raynium.client.mixin;

import dev.raynium.client.world.ParticleFastTrack;
import java.util.Arrays;
import net.minecraft.client.renderer.state.level.QuadParticleRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(QuadParticleRenderState.Storage.class)
abstract class QuadParticleStorageMixin implements ParticleFastTrack.PackedStorageAccess {
    @Shadow private int capacity;
    @Shadow private int currentParticleIndex;
    @Unique private int[] raynium$packedInts = new int[1024 * ParticleFastTrack.INSTANCE_INTS];

    @Override
    public int[] raynium$packedInts() {
        return raynium$packedInts;
    }

    @Inject(method = "grow", at = @At("RETURN"))
    private void raynium$growPacked(CallbackInfo ci) {
        raynium$packedInts = Arrays.copyOf(raynium$packedInts, this.capacity * ParticleFastTrack.INSTANCE_INTS);
    }

    @Inject(method = "add", at = @At("RETURN"))
    private void raynium$packOnAdd(
            float x, float y, float z,
            float xRot, float yRot, float zRot, float wRot,
            float scale, float u0, float u1, float v0, float v1,
            int color, int lightCoords, CallbackInfo ci) {
        int di = (this.currentParticleIndex - 1) * ParticleFastTrack.INSTANCE_INTS;
        int[] dst = raynium$packedInts;
        dst[di] = Float.floatToRawIntBits(x);
        dst[di + 1] = Float.floatToRawIntBits(y);
        dst[di + 2] = Float.floatToRawIntBits(z);
        dst[di + 3] = Float.floatToRawIntBits(scale);
        dst[di + 4] = Float.floatToRawIntBits(xRot);
        dst[di + 5] = Float.floatToRawIntBits(yRot);
        dst[di + 6] = Float.floatToRawIntBits(zRot);
        dst[di + 7] = Float.floatToRawIntBits(wRot);
        dst[di + 8] = Float.floatToRawIntBits(u0);
        dst[di + 9] = Float.floatToRawIntBits(u1);
        dst[di + 10] = Float.floatToRawIntBits(v0);
        dst[di + 11] = Float.floatToRawIntBits(v1);
        dst[di + 12] = color;
        dst[di + 13] = lightCoords;
    }
}
