package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeFrameBuffer;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * Off-heap pre-analyzed terrain mesh payload.
 * Solid/Cutout quads are classified into 6 directional groups + general (7th) and reordered on the
 * worker thread into a pooled DirectByteBuffer so the render thread uploads via zero-copy pointers.
 */
public final class MeshPayload {
    private static final ThreadLocal<byte[]> GROUP_SCRATCH = ThreadLocal.withInitial(() -> new byte[4096]);
    private static final AtomicReferenceFieldUpdater<MeshPayload, ByteBuffer> DIRECT_VERTICES_UPDATER =
            AtomicReferenceFieldUpdater.newUpdater(MeshPayload.class, ByteBuffer.class, "directVertices");

    /** Render-thread dense slot ID (1..maxSlot) assigned by TerrainResidency while staged/resident; 0 when unassigned. */
    public int slotId;
    public final long epoch;
    private volatile ByteBuffer directVertices;
    public final int vertexCount;
    public final int mode;
    public final int[] groupQuadCounts = new int[7];
    public final float[] minBounds = new float[3];
    public final float[] maxBounds = new float[3];
    public final int layoutFlags;
    public byte[] originalQuadGroups;
    public volatile IndexSnapshot indices;

    public static final class IndexSnapshot {
        private static final AtomicReferenceFieldUpdater<IndexSnapshot, ByteBuffer> DIRECT_BUF_UPDATER =
                AtomicReferenceFieldUpdater.newUpdater(IndexSnapshot.class, ByteBuffer.class, "directBuffer");
        public final int count;
        private final int[] heapValues;
        private volatile ByteBuffer directBuffer;

        public IndexSnapshot(int[] values) {
            this.count = values.length;
            this.heapValues = values;
            this.directBuffer = null;
        }

        IndexSnapshot(ByteBuffer directBuffer, int count) {
            this.count = count;
            this.heapValues = null;
            this.directBuffer = directBuffer;
        }

        boolean hasData() { return heapValues!=null || directBuffer!=null; }

        public int byteSize() {
            return count * 4;
        }

        public ByteBuffer directBuffer() {
            return directBuffer;
        }

        public int[] values() {
            if (heapValues != null) return heapValues;
            ByteBuffer buf = directBuffer;
            if (buf == null) return new int[0];
            int[] out = new int[count];
            for (int i = 0; i < count; i++) out[i] = buf.getInt(i * 4);
            return out;
        }

        public void releaseBuffer() {
            ByteBuffer buf = DIRECT_BUF_UPDATER.getAndSet(this, null);
            if (buf != null) DirectUploadPool.release(buf);
        }
    }

    public MeshPayload(long epoch, ByteBuffer source) {
        this(epoch, source, 0);
    }

    public MeshPayload(long epoch, ByteBuffer source, int mode) {
        if (mode < 0 || mode > 2) throw new IllegalArgumentException("Invalid terrain material mode: " + mode);
        this.epoch = epoch;
        this.mode = mode;
        ByteBuffer data = source.slice().order(ByteOrder.nativeOrder());
        int byteSize = data.remaining();
        if (byteSize % 112 != 0) throw new IllegalArgumentException("Terrain mesh is not a 28-byte quad stream");
        int quadCount = byteSize / 112;
        this.vertexCount = quadCount * 4;
        if (quadCount == 0) {
            this.layoutFlags = 0;
            this.directVertices = null;
            return;
        }

        boolean finite = true;
        float minX = Float.POSITIVE_INFINITY, minY = Float.POSITIVE_INFINITY, minZ = Float.POSITIVE_INFINITY;
        float maxX = Float.NEGATIVE_INFINITY, maxY = Float.NEGATIVE_INFINITY, maxZ = Float.NEGATIVE_INFINITY;

        if (mode == 2) {
            // Translucent preserves compiler quad order; only compute bounds and copy into direct buffer.
            for (int q = 0; q < quadCount; q++) {
                int base = q * 112;
                for (int v = 0; v < 4; v++) {
                    int vo = base + v * 28;
                    float x = data.getFloat(vo);
                    float y = data.getFloat(vo + 4);
                    float z = data.getFloat(vo + 8);
                    if (!Float.isFinite(x) || !Float.isFinite(y) || !Float.isFinite(z)) {
                        finite = false;
                    } else {
                        if (x < minX) minX = x;
                        if (x > maxX) maxX = x;
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                        if (z < minZ) minZ = z;
                        if (z > maxZ) maxZ = z;
                    }
                }
            }
            groupQuadCounts[6] = quadCount;
            ByteBuffer direct = DirectUploadPool.acquire(byteSize);
            MemorySegment.copy(MemorySegment.ofBuffer(data), 0L, MemorySegment.ofBuffer(direct), 0L, byteSize);
            this.directVertices = direct;
            if (finite) {
                minBounds[0] = minX; minBounds[1] = minY; minBounds[2] = minZ;
                maxBounds[0] = maxX; maxBounds[1] = maxY; maxBounds[2] = maxZ;
                this.layoutFlags = NativeFrameBuffer.LAYOUT_FLAG_FINITE_BOUNDS;
            } else {
                this.layoutFlags = 0;
            }
            return;
        }

        byte[] groups = GROUP_SCRATCH.get();
        if (groups.length < quadCount) {
            int cap = Math.max(4096, groups.length);
            while (cap < quadCount) cap = Math.multiplyExact(cap, 2);
            groups = new byte[cap];
            GROUP_SCRATCH.set(groups);
        }

        for (int q = 0; q < quadCount; q++) {
            int base = q * 112;
            float x0 = data.getFloat(base), y0 = data.getFloat(base + 4), z0 = data.getFloat(base + 8);
            float x1 = data.getFloat(base + 28), y1 = data.getFloat(base + 32), z1 = data.getFloat(base + 36);
            float x2 = data.getFloat(base + 56), y2 = data.getFloat(base + 60), z2 = data.getFloat(base + 64);
            float x3 = data.getFloat(base + 84), y3 = data.getFloat(base + 88), z3 = data.getFloat(base + 92);

            int g = 6;
            if (Float.isFinite(x0) && Float.isFinite(y0) && Float.isFinite(z0)
                    && Float.isFinite(x1) && Float.isFinite(y1) && Float.isFinite(z1)
                    && Float.isFinite(x2) && Float.isFinite(y2) && Float.isFinite(z2)
                    && Float.isFinite(x3) && Float.isFinite(y3) && Float.isFinite(z3)) {
                minX = Math.min(minX, Math.min(Math.min(x0, x1), Math.min(x2, x3)));
                maxX = Math.max(maxX, Math.max(Math.max(x0, x1), Math.max(x2, x3)));
                minY = Math.min(minY, Math.min(Math.min(y0, y1), Math.min(y2, y3)));
                maxY = Math.max(maxY, Math.max(Math.max(y0, y1), Math.max(y2, y3)));
                minZ = Math.min(minZ, Math.min(Math.min(z0, z1), Math.min(z2, z3)));
                maxZ = Math.max(maxZ, Math.max(Math.max(z0, z1), Math.max(z2, z3)));
                g = classifyQuad(x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3);
            } else {
                finite = false;
            }
            groups[q] = (byte) g;
            groupQuadCounts[g]++;
        }

        int nonEmptyGroups = 0;
        for (int g = 0; g < 7; g++) {
            if (groupQuadCounts[g] != 0) nonEmptyGroups++;
        }

        ByteBuffer direct = DirectUploadPool.acquire(byteSize);
        MemorySegment srcSeg = MemorySegment.ofBuffer(data);
        MemorySegment dstSeg = MemorySegment.ofBuffer(direct);
        if (nonEmptyGroups <= 1) {
            MemorySegment.copy(srcSeg, 0L, dstSeg, 0L, byteSize);
        } else {
            int[] offsets = new int[7];
            int cursor = 0;
            for (int g = 0; g < 7; g++) {
                offsets[g] = cursor;
                cursor += groupQuadCounts[g] * 112;
            }
            for (int q = 0; q < quadCount; q++) {
                int g = groups[q] & 7;
                int dstOff = offsets[g];
                offsets[g] = dstOff + 112;
                MemorySegment.copy(srcSeg, q * 112L, dstSeg, dstOff, 112L);
            }
            byte[] packed = new byte[(quadCount * 3 + 7) / 8];
            for (int q = 0; q < quadCount; q++) {
                int g = groups[q] & 7;
                int bit = q * 3;
                int b = bit >>> 3;
                int shift = bit & 7;
                packed[b] |= (byte) (g << shift);
                if (shift > 5) packed[b + 1] |= (byte) (g >>> (8 - shift));
            }
            this.originalQuadGroups = packed;
        }
        this.directVertices = direct;
        int flags = 0;
        if (finite) {
            minBounds[0] = minX; minBounds[1] = minY; minBounds[2] = minZ;
            maxBounds[0] = maxX; maxBounds[1] = maxY; maxBounds[2] = maxZ;
            flags |= NativeFrameBuffer.LAYOUT_FLAG_FINITE_BOUNDS;
        }
        if (this.originalQuadGroups != null) {
            flags |= NativeFrameBuffer.LAYOUT_FLAG_WIREFRAME_GROUPS;
        }
        this.layoutFlags = flags;
    }

    /** Exact mirror of native TerrainQuadGroup in terrain_draw.cpp. */
    private static int classifyQuad(
            float x0, float y0, float z0,
            float x1, float y1, float z1,
            float x2, float y2, float z2,
            float x3, float y3, float z3) {
        if (x0 == x1 && x0 == x2 && x0 == x3) {
            double b1 = (double) y1 - y0, c1 = (double) z1 - z0;
            double b2 = (double) y2 - y0, c2 = (double) z2 - z0;
            double b3 = (double) y3 - y0, c3 = (double) z3 - z0;
            double n = b1 * c2 - c1 * b2;
            double m = b2 * c3 - c2 * b3;
            if (n > 0.0 && m > 0.0) return 0;
            if (n < 0.0 && m < 0.0) return 1;
        }
        if (y0 == y1 && y0 == y2 && y0 == y3) {
            double b1 = (double) z1 - z0, c1 = (double) x1 - x0;
            double b2 = (double) z2 - z0, c2 = (double) x2 - x0;
            double b3 = (double) z3 - z0, c3 = (double) x3 - x0;
            double n = b1 * c2 - c1 * b2;
            double m = b2 * c3 - c2 * b3;
            if (n > 0.0 && m > 0.0) return 2;
            if (n < 0.0 && m < 0.0) return 3;
        }
        if (z0 == z1 && z0 == z2 && z0 == z3) {
            double b1 = (double) x1 - x0, c1 = (double) y1 - y0;
            double b2 = (double) x2 - x0, c2 = (double) y2 - y0;
            double b3 = (double) x3 - x0, c3 = (double) y3 - y0;
            double n = b1 * c2 - c1 * b2;
            double m = b2 * c3 - c2 * b3;
            if (n > 0.0 && m > 0.0) return 4;
            if (n < 0.0 && m < 0.0) return 5;
        }
        return 6;
    }

    public ByteBuffer directVertices() {
        return directVertices;
    }

    public boolean hasVertices() {
        return directVertices != null;
    }

    public void releaseVertices() {
        ByteBuffer buf = DIRECT_VERTICES_UPDATER.getAndSet(this, null);
        if (buf != null) DirectUploadPool.release(buf);
        originalQuadGroups = null;
    }

    public IndexSnapshot copyIndices(ByteBuffer source, int stride) {
        ByteBuffer data = source.slice().order(ByteOrder.nativeOrder());
        int count = vertexCount / 4 * 6;
        if ((stride != 2 && stride != 4) || data.remaining() != count * stride)
            throw new IllegalArgumentException("Invalid transparent indices");
        ByteBuffer direct = DirectUploadPool.acquire(count * 4);
        try {
            if (stride == 2) {
                for (int i = 0; i < count; i++) {
                    int value = Short.toUnsignedInt(data.getShort(i * 2));
                    if (value >= vertexCount) throw new IllegalArgumentException("Transparent index outside mesh");
                    direct.putInt(i * 4, value);
                }
            } else {
                for (int i = 0; i < count; i++) {
                    int value = data.getInt(i * 4);
                    if (value < 0 || value >= vertexCount) throw new IllegalArgumentException("Transparent index outside mesh");
                    direct.putInt(i * 4, value);
                }
            }
            return new IndexSnapshot(direct, count);
        } catch (RuntimeException | Error failure) {
            DirectUploadPool.release(direct);
            throw failure;
        }
    }
}
