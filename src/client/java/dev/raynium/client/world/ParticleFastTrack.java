package dev.raynium.client.world;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.IdentityHashMap;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.StagedVertexBuffer;
import net.minecraft.client.renderer.oit.OitPipelineSet;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.texture.AbstractTexture;
import org.jspecify.annotations.Nullable;

/** Packs 56 B/particle compact instance streams directly into the dedicated CommandTape instance stream (Opcode 16). */
public final class ParticleFastTrack {
    public static final int INSTANCE_BYTES = 56;
    public static final int INSTANCE_INTS = 14;
    private static final int[] QUAD = {1}, FIRST_INSTANCE = {0};
    private static final int MAX_POOLED_BATCHES = 64;
    private static final ArrayDeque<CompactLayerBatch> BATCH_POOL = new ArrayDeque<>(16);

    public interface RawParticleAccess {
        int raynium$appendCompactLayer(SingleQuadParticle.Layer layer, CompactLayerBatch out);
    }

    public interface PackedStorageAccess {
        int[] raynium$packedInts();
    }

    private ParticleFastTrack() {}

    public static final class PreparedLayers {
        public final IdentityHashMap<SingleQuadParticle.Layer, StagedVertexBuffer.Draw> draws = new IdentityHashMap<>();
        public final IdentityHashMap<SingleQuadParticle.Layer, AbstractTexture> textures = new IdentityHashMap<>();
        public final IdentityHashMap<SingleQuadParticle.Layer, CompactLayerBatch> compact = new IdentityHashMap<>();

        public void clear() {
            compact.forEach((layer, batch) -> batch.recycle());
            compact.clear();
            draws.clear();
            textures.clear();
        }
    }

    public static CompactLayerBatch acquireBatch(SingleQuadParticle.Layer layer) {
        CompactLayerBatch batch = BATCH_POOL.pollFirst();
        if (batch == null) {
            batch = new CompactLayerBatch();
        }
        batch.init(layer);
        return batch;
    }

    static void recycleBatch(CompactLayerBatch batch) {
        batch.reset();
        if (BATCH_POOL.size() < MAX_POOLED_BATCHES) {
            BATCH_POOL.addFirst(batch);
        }
    }

    public static boolean isLayerEligible(RayniumGpuDevice device, SingleQuadParticle.Layer layer, boolean useOit) {
        if (!device.supportsInstanced(layer.pipeline(), "particle")) return false;
        if (layer.translucent() && useOit) {
            OitPipelineSet oitSet = layer.oitPipelineSet();
            if (oitSet == null) return false;
            return device.supportsInstanced(oitSet.getPipeline(OitStage.DEPTH_BOUNDS), "particle")
                && device.supportsInstanced(oitSet.getPipeline(OitStage.TRANSMITTANCE), "particle")
                && device.supportsInstanced(oitSet.getPipeline(OitStage.ACCUMULATE), "particle");
        }
        return true;
    }

    /**
     * Packs count particles from QuadParticleRenderState.Storage arrays into 14 little-endian ints (56 B) each:
     *   [0..3]  InstPosScale = (x, y, z, scale)
     *   [4..7]  InstRot      = (xRot, yRot, zRot, wRot)
     *   [8..11] InstUvRect   = (u0, u1, v0, v1)
     *   [12]    InstColor    = raw ARGB (shader reads InstColor.bgra)
     *   [13]    InstUV2      = lightCoords
     */
    public static void packParticles(float[] floatValues, int[] intValues, int count, int[] dstInts, int dstIntOffset) {
        for (int i = 0; i < count; ++i) {
            int fi = i * 12;
            int ii = i * 2;
            int di = dstIntOffset + i * INSTANCE_INTS;
            dstInts[di] = Float.floatToRawIntBits(floatValues[fi]);
            dstInts[di + 1] = Float.floatToRawIntBits(floatValues[fi + 1]);
            dstInts[di + 2] = Float.floatToRawIntBits(floatValues[fi + 2]);
            dstInts[di + 3] = Float.floatToRawIntBits(floatValues[fi + 7]);
            dstInts[di + 4] = Float.floatToRawIntBits(floatValues[fi + 3]);
            dstInts[di + 5] = Float.floatToRawIntBits(floatValues[fi + 4]);
            dstInts[di + 6] = Float.floatToRawIntBits(floatValues[fi + 5]);
            dstInts[di + 7] = Float.floatToRawIntBits(floatValues[fi + 6]);
            dstInts[di + 8] = Float.floatToRawIntBits(floatValues[fi + 8]);
            dstInts[di + 9] = Float.floatToRawIntBits(floatValues[fi + 9]);
            dstInts[di + 10] = Float.floatToRawIntBits(floatValues[fi + 10]);
            dstInts[di + 11] = Float.floatToRawIntBits(floatValues[fi + 11]);
            dstInts[di + 12] = intValues[ii];
            dstInts[di + 13] = intValues[ii + 1];
        }
    }

    /** Packs count particles directly from Storage arrays into off-heap CommandTape upload memory with zero heap copy. */
    public static void packParticlesToSegment(float[] floatValues, int[] intValues, int count,
            MemorySegment dst, long dstByteOffset) {
        long off = dstByteOffset;
        for (int i = 0, fi = 0, ii = 0; i < count; ++i, fi += 12, ii += 2, off += INSTANCE_BYTES) {
            dst.set(ValueLayout.JAVA_FLOAT, off, floatValues[fi]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 4, floatValues[fi + 1]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 8, floatValues[fi + 2]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 12, floatValues[fi + 7]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 16, floatValues[fi + 3]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 20, floatValues[fi + 4]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 24, floatValues[fi + 5]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 28, floatValues[fi + 6]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 32, floatValues[fi + 8]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 36, floatValues[fi + 9]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 40, floatValues[fi + 10]);
            dst.set(ValueLayout.JAVA_FLOAT, off + 44, floatValues[fi + 11]);
            dst.set(ValueLayout.JAVA_INT, off + 48, intValues[ii]);
            dst.set(ValueLayout.JAVA_INT, off + 52, intValues[ii + 1]);
        }
    }

    public static final class CompactLayerBatch {
        private final int[] runCount = new int[1];
        private SingleQuadParticle.Layer layer;
        private AbstractTexture texture;
        private float[][] sliceFloats = new float[4][];
        private int[][] sliceInts = new int[4][];
        private int[][] slicePackedInts = new int[4][];
        private int[] sliceCounts = new int[4];
        private int sliceCount;
        private int particleCount;
        private @Nullable CommandTape uploadedTape;
        private long uploadedOffset;

        CompactLayerBatch() {}

        public CompactLayerBatch(SingleQuadParticle.Layer layer) {
            this.layer = layer;
        }

        void init(SingleQuadParticle.Layer layer) {
            this.layer = layer;
            this.texture = null;
            this.sliceCount = 0;
            this.particleCount = 0;
            this.uploadedTape = null;
            this.uploadedOffset = 0;
        }

        void reset() {
            for (int i = 0; i < sliceCount; ++i) {
                sliceFloats[i] = null;
                sliceInts[i] = null;
                slicePackedInts[i] = null;
            }
            layer = null;
            texture = null;
            sliceCount = 0;
            particleCount = 0;
            uploadedTape = null;
            uploadedOffset = 0;
        }

        public void recycle() {
            recycleBatch(this);
        }

        public void setTexture(AbstractTexture texture) { this.texture = texture; }
        public int particleCount() { return particleCount; }

        private void ensureSliceCapacity() {
            if (sliceCount == sliceCounts.length) {
                int next = sliceCounts.length * 2;
                sliceFloats = Arrays.copyOf(sliceFloats, next);
                sliceInts = Arrays.copyOf(sliceInts, next);
                slicePackedInts = Arrays.copyOf(slicePackedInts, next);
                sliceCounts = Arrays.copyOf(sliceCounts, next);
            }
        }

        public void append(float[] floatValues, int[] intValues, int count) {
            if (count <= 0) return;
            ensureSliceCapacity();
            sliceFloats[sliceCount] = floatValues;
            sliceInts[sliceCount] = intValues;
            slicePackedInts[sliceCount] = null;
            sliceCounts[sliceCount] = count;
            ++sliceCount;
            particleCount += count;
        }

        public void appendPacked(int[] packedInts, int count) {
            if (count <= 0) return;
            ensureSliceCapacity();
            sliceFloats[sliceCount] = null;
            sliceInts[sliceCount] = null;
            slicePackedInts[sliceCount] = packedInts;
            sliceCounts[sliceCount] = count;
            ++sliceCount;
            particleCount += count;
        }

        public void execute(RenderPass renderPass, @Nullable OitStage stage) {
            if (particleCount <= 0) return;
            CommandTape tape = RayniumGpuDevice.activeTape(renderPass);
            if (uploadedTape != tape) {
                long totalBytes = (long) particleCount * INSTANCE_BYTES;
                long offset = tape.reserveInstanceUpload(totalBytes, 8);
                MemorySegment dst = tape.instanceUploadSlice(offset, totalBytes);
                long dstByteOffset = 0L;
                for (int s = 0; s < sliceCount; ++s) {
                    int count = sliceCounts[s];
                    int[] packed = slicePackedInts[s];
                    if (packed != null) {
                        MemorySegment.copy(packed, 0, dst, ValueLayout.JAVA_INT, dstByteOffset, count * INSTANCE_INTS);
                    } else {
                        packParticlesToSegment(sliceFloats[s], sliceInts[s], count, dst, dstByteOffset);
                    }
                    dstByteOffset += (long) count * INSTANCE_BYTES;
                }
                uploadedTape = tape;
                uploadedOffset = offset;
            }
            RenderPipeline targetPipeline;
            if (stage != null) {
                OitPipelineSet oitSet = layer.oitPipelineSet();
                if (oitSet == null) throw new IllegalStateException("OIT pipeline set for particle layer not specified.");
                targetPipeline = oitSet.getPipeline(stage);
            } else {
                targetPipeline = layer.pipeline();
            }
            renderPass.setPipeline(RenderSystem.getCompiledPipeline(targetPipeline));
            renderPass.setUniform("Sampler0", texture.getTextureView(), texture.getSampler());
            runCount[0] = particleCount;
            if (!RayniumGpuDevice.recordInstancedGeometry(renderPass, "particle", uploadedOffset, particleCount, INSTANCE_BYTES, QUAD, FIRST_INSTANCE, runCount, 1)) {
                throw new IllegalStateException("Instanced compact particle pipeline unavailable during execute for " + targetPipeline.getLocation());
            }
        }
    }
}
