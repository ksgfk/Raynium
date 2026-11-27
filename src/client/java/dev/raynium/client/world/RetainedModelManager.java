package dev.raynium.client.world;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.IdentityHashMap;
import java.util.List;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.util.ARGB;
import net.minecraft.util.Mth;
import org.joml.Matrix3f;
import org.joml.Matrix4f;
import org.joml.Vector3f;
import org.joml.Vector3fc;

/** Manages GPU-resident 24 B/vertex local meshes for rigid ModelParts and 80 B instance packing. */
public final class RetainedModelManager {
    public static final int LOCAL_VERTEX_BYTES = 24;
    public static final int INSTANCE_BYTES = 80;
    public static final int INSTANCE_INTS = 20;

    public interface PoseAccess {
        boolean raynium$trustedNormals();
    }

    public interface PartHandle {
        long raynium$getMeshEpoch();
        int raynium$getMeshId();
        int raynium$getMeshPage();
        void raynium$setMeshCache(long epoch, int meshId, int page);
        List<ModelPart.Cube> raynium$getCubes();
    }

    private static final class ModelRef extends WeakReference<ModelPart> {
        final int meshId;
        final long epoch;
        ModelRef(ModelPart referent, ReferenceQueue<ModelPart> queue, int meshId, long epoch) {
            super(referent, queue);
            this.meshId = meshId;
            this.epoch = epoch;
        }
    }

    private static NativeRenderer activeRenderer;
    private static long epoch = 1L;
    private static final int SEEN_ONCE = -2;
    private static final it.unimi.dsi.fastutil.ints.IntArrayList FREE_MESH_IDS = new it.unimi.dsi.fastutil.ints.IntArrayList();
    private static int nextMeshId = 16;
    private static final ReferenceQueue<ModelPart> GC_QUEUE = new ReferenceQueue<>();
    private static IdentityHashMap<ModelRef, Boolean> LIVE_REFS = new IdentityHashMap<>();

    private RetainedModelManager() {}

    /**
     * Extracts 24 B/vertex local mesh bytes (float3 localPos/16, float2 uv0, snorm8x4 localNormal).
     * Returns null if empty or if any polygon is non-quad or has a non-axial unit normal.
     */
    public static byte[] extractLocalMesh(List<ModelPart.Cube> cubes) {
        if (cubes == null || cubes.isEmpty()) return null;
        int quadCount = 0;
        for (int c = 0; c < cubes.size(); ++c) {
            ModelPart.Polygon[] polygons = cubes.get(c).polygons;
            if (polygons == null) return null;
            for (ModelPart.Polygon polygon : polygons) {
                if (polygon == null || polygon.vertices().length != 4 || !isAxialUnitNormal(polygon.normal())) {
                    return null;
                }
                ++quadCount;
            }
        }
        if (quadCount == 0) return null;
        int vertexCount = Math.multiplyExact(quadCount, 4);
        byte[] out = new byte[Math.multiplyExact(vertexCount, LOCAL_VERTEX_BYTES)];
        ByteBuffer buf = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        for (int c = 0; c < cubes.size(); ++c) {
            for (ModelPart.Polygon polygon : cubes.get(c).polygons) {
                Vector3fc normal = polygon.normal();
                byte nx = encodeAxisByte(normal.x());
                byte ny = encodeAxisByte(normal.y());
                byte nz = encodeAxisByte(normal.z());
                for (ModelPart.Vertex v : polygon.vertices()) {
                    buf.putFloat(v.worldX());
                    buf.putFloat(v.worldY());
                    buf.putFloat(v.worldZ());
                    buf.putFloat(v.u());
                    buf.putFloat(v.v());
                    buf.put(nx);
                    buf.put(ny);
                    buf.put(nz);
                    buf.put((byte) 0);
                }
            }
        }
        return out;
    }

    public static boolean isAxialUnitNormal(Vector3fc n) {
        if (n == null) return false;
        float x = n.x(), y = n.y(), z = n.z();
        return (Math.abs(x) == 1.0F && y == 0.0F && z == 0.0F)
            || (x == 0.0F && Math.abs(y) == 1.0F && z == 0.0F)
            || (x == 0.0F && y == 0.0F && Math.abs(z) == 1.0F);
    }

    private static byte encodeAxisByte(float component) {
        return component > 0.0F ? (byte) 127 : component < 0.0F ? (byte) -127 : (byte) 0;
    }

    /** Matches BufferBuilder.normalIntValue bit-for-bit. */
    public static byte normalIntValue(float c) {
        return (byte) ((int) (Mth.clamp(c, -1.0F, 1.0F) * 127.0F) & 0xFF);
    }

    public static int packNormalByte4(float nx, float ny, float nz) {
        int b0 = normalIntValue(nx) & 0xFF;
        int b1 = normalIntValue(ny) & 0xFF;
        int b2 = normalIntValue(nz) & 0xFF;
        return b0 | (b1 << 8) | (b2 << 16);
    }

    private static int packNormalizedColumn(float x, float y, float z) {
        float inverse=org.joml.Math.invsqrt(org.joml.Math.fma(x,x,org.joml.Math.fma(y,y,z*z)));
        return packNormalByte4(x*inverse,y*inverse,z*inverse);
    }

    /** Packs one 80-byte ModelInstance record (20 little-endian ints) directly from Matrix4f/Matrix3f columns. */
    public static void packInstanceDirect(Matrix4f m, Matrix3f n, boolean trustedNormals,
            int colorArgb, int overlayCoords, int lightCoords, int[] dst, int intOffset, Vector3f scratch) {
        dst[intOffset] = Float.floatToRawIntBits(m.m00());
        dst[intOffset + 1] = Float.floatToRawIntBits(m.m10());
        dst[intOffset + 2] = Float.floatToRawIntBits(m.m20());
        dst[intOffset + 3] = Float.floatToRawIntBits(m.m30());
        dst[intOffset + 4] = Float.floatToRawIntBits(m.m01());
        dst[intOffset + 5] = Float.floatToRawIntBits(m.m11());
        dst[intOffset + 6] = Float.floatToRawIntBits(m.m21());
        dst[intOffset + 7] = Float.floatToRawIntBits(m.m31());
        dst[intOffset + 8] = Float.floatToRawIntBits(m.m02());
        dst[intOffset + 9] = Float.floatToRawIntBits(m.m12());
        dst[intOffset + 10] = Float.floatToRawIntBits(m.m22());
        dst[intOffset + 11] = Float.floatToRawIntBits(m.m32());
        dst[intOffset + 12] = ARGB.toABGR(colorArgb);
        dst[intOffset + 13] = overlayCoords;
        dst[intOffset + 14] = lightCoords;
        if (trustedNormals) {
            dst[intOffset + 15] = packNormalByte4(n.m00(), n.m01(), n.m02());
            dst[intOffset + 16] = packNormalByte4(n.m10(), n.m11(), n.m12());
            dst[intOffset + 17] = packNormalByte4(n.m20(), n.m21(), n.m22());
        } else {
            dst[intOffset + 15] = packNormalizedColumn(n.m00(), n.m01(), n.m02());
            dst[intOffset + 16] = packNormalizedColumn(n.m10(), n.m11(), n.m12());
            dst[intOffset + 17] = packNormalizedColumn(n.m20(), n.m21(), n.m22());
        }
        dst[intOffset + 18] = 0;
        dst[intOffset + 19] = 0;
    }

    /** Packs one 80-byte ModelInstance record (20 little-endian ints) with a local pure translation (tx, ty, tz). */
    public static void packInstanceTranslated(PoseStack.Pose pose, float tx, float ty, float tz,
            int colorArgb, int overlayCoords, int lightCoords, int[] dst, int intOffset, Vector3f scratch) {
        packInstance(pose, colorArgb, overlayCoords, lightCoords, dst, intOffset, scratch);
        Matrix4f m = pose.pose();
        float m30 = org.joml.Math.fma(m.m00(), tx, org.joml.Math.fma(m.m10(), ty, org.joml.Math.fma(m.m20(), tz, m.m30())));
        float m31 = org.joml.Math.fma(m.m01(), tx, org.joml.Math.fma(m.m11(), ty, org.joml.Math.fma(m.m21(), tz, m.m31())));
        float m32 = org.joml.Math.fma(m.m02(), tx, org.joml.Math.fma(m.m12(), ty, org.joml.Math.fma(m.m22(), tz, m.m32())));
        dst[intOffset + 3] = Float.floatToRawIntBits(m30);
        dst[intOffset + 7] = Float.floatToRawIntBits(m31);
        dst[intOffset + 11] = Float.floatToRawIntBits(m32);
    }

    /** Packs one 80-byte ModelInstance record (20 little-endian ints). */
    public static void packInstance(PoseStack.Pose pose, int colorArgb, int overlayCoords, int lightCoords,
            int[] dst, int intOffset, Vector3f scratch) {
        if ((Object) pose instanceof PoseAccess access) {
            packInstanceDirect(pose.pose(), pose.normal(), access.raynium$trustedNormals(),
                colorArgb, overlayCoords, lightCoords, dst, intOffset, scratch);
            return;
        }
        Matrix4f m = pose.pose();
        dst[intOffset] = Float.floatToRawIntBits(m.m00());
        dst[intOffset + 1] = Float.floatToRawIntBits(m.m10());
        dst[intOffset + 2] = Float.floatToRawIntBits(m.m20());
        dst[intOffset + 3] = Float.floatToRawIntBits(m.m30());
        dst[intOffset + 4] = Float.floatToRawIntBits(m.m01());
        dst[intOffset + 5] = Float.floatToRawIntBits(m.m11());
        dst[intOffset + 6] = Float.floatToRawIntBits(m.m21());
        dst[intOffset + 7] = Float.floatToRawIntBits(m.m31());
        dst[intOffset + 8] = Float.floatToRawIntBits(m.m02());
        dst[intOffset + 9] = Float.floatToRawIntBits(m.m12());
        dst[intOffset + 10] = Float.floatToRawIntBits(m.m22());
        dst[intOffset + 11] = Float.floatToRawIntBits(m.m32());
        dst[intOffset + 12] = ARGB.toABGR(colorArgb);
        dst[intOffset + 13] = overlayCoords;
        dst[intOffset + 14] = lightCoords;
        if(scratch==null) scratch=new Vector3f();
        Vector3f nx = pose.transformNormal(1.0F, 0.0F, 0.0F, scratch);
        dst[intOffset + 15] = packNormalByte4(nx.x(), nx.y(), nx.z());
        Vector3f ny = pose.transformNormal(0.0F, 1.0F, 0.0F, scratch);
        dst[intOffset + 16] = packNormalByte4(ny.x(), ny.y(), ny.z());
        Vector3f nz = pose.transformNormal(0.0F, 0.0F, 1.0F, scratch);
        dst[intOffset + 17] = packNormalByte4(nz.x(), nz.y(), nz.z());
        dst[intOffset + 18] = 0;
        dst[intOffset + 19] = 0;
    }

    static boolean hasResidentMesh(RayniumGpuDevice device,PartHandle handle) {
        return activeRenderer==device.nativeRenderer() && handle.raynium$getMeshEpoch()==epoch && handle.raynium$getMeshId()>0;
    }

    // A return value of SEEN_ONCE releases the caller to extract and register the mesh.
    static int promotionState(PartHandle handle,long currentEpoch) {
        if(handle.raynium$getMeshEpoch()!=currentEpoch) {
            handle.raynium$setMeshCache(currentEpoch, SEEN_ONCE, 0);
            return -1;
        }
        return handle.raynium$getMeshId();
    }

    public static int resolveMeshId(RayniumGpuDevice device, ModelPart part) {
        NativeRenderer renderer = device.nativeRenderer();
        if (activeRenderer != renderer) {
            LIVE_REFS = new IdentityHashMap<>();
            drainQueueOnly();
            activeRenderer = renderer;
            ++epoch;
            nextMeshId = 16;
            FREE_MESH_IDS.clear();
            FREE_MESH_IDS.trim();
        }
        drainGc(renderer);
        PartHandle handle = (PartHandle) (Object) part;
        int state = promotionState(handle, epoch);
        if (state != SEEN_ONCE) return state;
        List<ModelPart.Cube> cubes = handle.raynium$getCubes();
        if (cubes.isEmpty()) {
            handle.raynium$setMeshCache(epoch, 0, 0);
            return 0;
        }
        byte[] bytes = extractLocalMesh(cubes);
        if (bytes == null) {
            handle.raynium$setMeshCache(epoch, -1, 0);
            return -1;
        }
        int vertexCount = bytes.length / LOCAL_VERTEX_BYTES;
        int indexCount = (vertexCount / 4) * 6;
        if (FREE_MESH_IDS.isEmpty() && nextMeshId == Integer.MAX_VALUE) {
            handle.raynium$setMeshCache(epoch, -1, 0);
            return -1;
        }
        int id = FREE_MESH_IDS.isEmpty() ? nextMeshId++ : FREE_MESH_IDS.popInt();
        int page = renderer.upsertModelMesh(id, bytes, vertexCount, indexCount);
        LIVE_REFS.put(new ModelRef(part, GC_QUEUE, id, epoch), Boolean.TRUE);
        handle.raynium$setMeshCache(epoch, id, page);
        return id;
    }

    public static void invalidateAll() {
        ModelFastTrack.clearCachedGeometry();
        ++epoch;
        nextMeshId = 16;
        FREE_MESH_IDS.clear();
        FREE_MESH_IDS.trim();
        NativeRenderer renderer = activeRenderer;
        if (renderer != null && !LIVE_REFS.isEmpty()) {
            for (ModelRef ref : LIVE_REFS.keySet()) {
                renderer.removeModelMesh(ref.meshId);
            }
        }
        LIVE_REFS = new IdentityHashMap<>();
        drainQueueOnly();
    }

    public static void onDeviceClose(NativeRenderer renderer) {
        if (activeRenderer == renderer) {
            ModelFastTrack.clearCachedGeometry();
            FREE_MESH_IDS.clear();
            FREE_MESH_IDS.trim();
            LIVE_REFS = new IdentityHashMap<>();
            drainQueueOnly();
            activeRenderer = null;
            ++epoch;
        }
    }

    private static void drainGc(NativeRenderer renderer) {
        ModelRef ref;
        while ((ref = (ModelRef) GC_QUEUE.poll()) != null) {
            if (LIVE_REFS.remove(ref) != null && ref.epoch == epoch) {
                renderer.removeModelMesh(ref.meshId);
                FREE_MESH_IDS.push(ref.meshId);
            }
        }
    }

    private static void drainQueueOnly() {
        while (GC_QUEUE.poll() != null) {}
    }
}
