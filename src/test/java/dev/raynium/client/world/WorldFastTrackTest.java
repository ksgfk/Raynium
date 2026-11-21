package dev.raynium.client.world;

import com.mojang.blaze3d.vertex.PoseStack;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.EnumSet;
import java.util.List;
import java.util.Random;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.core.Direction;
import net.minecraft.util.ARGB;
import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class WorldFastTrackTest {
    @Test
    void extractsLocalMeshAndRejectsNonQuadOrNonAxialPolygons() {
        ModelPart.Cube cube = new ModelPart.Cube(
            0, 0, -4.0F, -8.0F, -4.0F, 8.0F, 16.0F, 8.0F,
            0.0F, 0.0F, 0.0F, false, 64.0F, 64.0F, EnumSet.allOf(Direction.class)
        );
        byte[] bytes = RetainedModelManager.extractLocalMesh(List.of(cube));
        assertNotNull(bytes);
        // 6 faces * 4 vertices * 24 bytes = 576 bytes.
        assertEquals(6 * 4 * 24, bytes.length);
        ByteBuffer buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        for (int v = 0; v < 24; ++v) {
            float x = buf.getFloat();
            float y = buf.getFloat();
            float z = buf.getFloat();
            float u = buf.getFloat();
            float vCoord = buf.getFloat();
            byte nx = buf.get();
            byte ny = buf.get();
            byte nz = buf.get();
            byte nw = buf.get();
            assertTrue(Math.abs(x) <= 0.25F + 1e-6F);
            assertTrue(Math.abs(y) <= 0.5F + 1e-6F);
            assertTrue(Math.abs(z) <= 0.25F + 1e-6F);
            assertTrue(u >= 0.0F && u <= 1.0F && vCoord >= 0.0F && vCoord <= 1.0F);
            assertEquals(0, nw);
            int axisSum = Math.abs(nx) + Math.abs(ny) + Math.abs(nz);
            assertEquals(127, axisSum, "Each vertex normal must be strictly one signed unit axis (±127)");
        }

        // Empty list returns null.
        assertNull(RetainedModelManager.extractLocalMesh(List.of()));

        // Non-axial normal must fall back (return null).
        ModelPart.Cube nonAxial = new ModelPart.Cube(
            0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, false, 16, 16, EnumSet.of(Direction.UP)
        );
        ModelPart.Vertex[] quad = nonAxial.polygons[0].vertices();
        nonAxial.polygons[0] = new ModelPart.Polygon(quad, new Vector3f(0.6F, 0.8F, 0.0F));
        assertNull(RetainedModelManager.extractLocalMesh(List.of(nonAxial)));

        // Non-4-vertex polygon must fall back (return null).
        ModelPart.Cube tri = new ModelPart.Cube(
            0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, false, 16, 16, EnumSet.of(Direction.UP)
        );
        tri.polygons[0] = new ModelPart.Polygon(new ModelPart.Vertex[]{quad[0], quad[1], quad[2]}, Direction.UP.getUnitVec3f());
        assertNull(RetainedModelManager.extractLocalMesh(List.of(tri)));
    }

    @Test
    void modelInstanceNormalBasisAndPositionAreBitExactToVanilla() {
        Random rng = new Random(0x5241594EL);
        Direction[] dirs = Direction.values();
        int[] packed = new int[RetainedModelManager.INSTANCE_INTS];
        Vector3f scratch = new Vector3f();
        Vector3f vanillaNormal = new Vector3f();
        Vector3f vanillaPos = new Vector3f();

        for (int trial = 0; trial < 500; ++trial) {
            PoseStack stack = new PoseStack();
            stack.translate((rng.nextFloat() - 0.5F) * 20.0F, (rng.nextFloat() - 0.5F) * 20.0F, (rng.nextFloat() - 0.5F) * 20.0F);
            stack.rotate(new Quaternionf().rotationZYX(
                (rng.nextFloat() - 0.5F) * 6.28F,
                (rng.nextFloat() - 0.5F) * 6.28F,
                (rng.nextFloat() - 0.5F) * 6.28F
            ));
            int mode = trial % 5;
            if (mode == 1) {
                // Non-uniform scale.
                stack.scale(0.25F + rng.nextFloat() * 3.0F, 0.25F + rng.nextFloat() * 3.0F, 0.25F + rng.nextFloat() * 3.0F);
            } else if (mode == 2) {
                // Negative mirrored scale.
                stack.scale(-1.0F, 1.0F, -1.0F);
            } else if (mode == 3) {
                // Mixed negative non-uniform scale.
                stack.scale(-0.5F, 2.0F, -1.5F);
            } else if (mode == 4) {
                // Arbitrary shear / affine mulPose.
                Matrix4f shear = new Matrix4f().identity();
                shear.m10(0.35F);
                shear.m21(-0.2F);
                stack.mulPose(shear);
            }
            PoseStack.Pose pose = stack.last();
            int color = 0xAA336699;
            int overlay = 0x000A0005;
            int light = 0x00F000D0;
            RetainedModelManager.packInstance(pose, color, overlay, light, packed, 0, scratch);

            // Also verify direct matrix column read (P2.3) matches packInstance bit-for-bit across trusted & untrusted modes.
            boolean trustedNormals = (mode == 0 || mode == 2);
            int[] directPacked = new int[RetainedModelManager.INSTANCE_INTS];
            RetainedModelManager.packInstanceDirect(pose.pose(), pose.normal(), trustedNormals, color, overlay, light, directPacked, 0, scratch);
            assertArrayEquals(packed, directPacked, "Direct matrix column packing mismatch for mode=" + mode);

            assertEquals(ARGB.toABGR(color), packed[12]);
            assertEquals(overlay, packed[13]);
            assertEquals(light, packed[14]);
            assertEquals(0, packed[18]);
            assertEquals(0, packed[19]);

            // Verify position transform matches Matrix4f.transformPosition.
            float lx = (rng.nextFloat() - 0.5F) * 2.0F;
            float ly = (rng.nextFloat() - 0.5F) * 2.0F;
            float lz = (rng.nextFloat() - 0.5F) * 2.0F;
            pose.pose().transformPosition(lx, ly, lz, vanillaPos);
            float gx = Float.intBitsToFloat(packed[0]) * lx + Float.intBitsToFloat(packed[1]) * ly
                     + Float.intBitsToFloat(packed[2]) * lz + Float.intBitsToFloat(packed[3]);
            float gy = Float.intBitsToFloat(packed[4]) * lx + Float.intBitsToFloat(packed[5]) * ly
                     + Float.intBitsToFloat(packed[6]) * lz + Float.intBitsToFloat(packed[7]);
            float gz = Float.intBitsToFloat(packed[8]) * lx + Float.intBitsToFloat(packed[9]) * ly
                     + Float.intBitsToFloat(packed[10]) * lz + Float.intBitsToFloat(packed[11]);
            assertEquals(vanillaPos.x, gx, 1e-5F);
            assertEquals(vanillaPos.y, gy, 1e-5F);
            assertEquals(vanillaPos.z, gz, 1e-5F);

            // Verify all 6 unit axes evaluate identically to vanilla CPU BufferBuilder.normalIntValue.
            for (Direction dir : dirs) {
                var axis = dir.getUnitVec3f();
                pose.transformNormal(axis, vanillaNormal);
                byte expX = RetainedModelManager.normalIntValue(vanillaNormal.x);
                byte expY = RetainedModelManager.normalIntValue(vanillaNormal.y);
                byte expZ = RetainedModelManager.normalIntValue(vanillaNormal.z);

                float localX = axis.x();
                float localY = axis.y();
                float localZ = axis.z();
                float gpuNx = localX * unpackSnorm8(packed[15], 0) + localY * unpackSnorm8(packed[16], 0) + localZ * unpackSnorm8(packed[17], 0);
                float gpuNy = localX * unpackSnorm8(packed[15], 1) + localY * unpackSnorm8(packed[16], 1) + localZ * unpackSnorm8(packed[17], 1);
                float gpuNz = localX * unpackSnorm8(packed[15], 2) + localY * unpackSnorm8(packed[16], 2) + localZ * unpackSnorm8(packed[17], 2);
                assertEquals(expX / 127.0F, gpuNx, 0.0F, "Bit-exact normal X mismatch for " + dir);
                assertEquals(expY / 127.0F, gpuNy, 0.0F, "Bit-exact normal Y mismatch for " + dir);
                assertEquals(expZ / 127.0F, gpuNz, 0.0F, "Bit-exact normal Z mismatch for " + dir);
            }
        }
    }

    @Test
    void compactParticlePackingAndQuaternionFormulaMatchVanilla() {
        float[] floats = new float[]{
            1.5F, -2.25F, 3.75F,     // x, y, z
            0.3F, -0.4F, 0.5F, 1.2F, // intentionally non-unit quaternion (xRot, yRot, zRot, wRot)
            0.75F,                   // scale
            0.125F, 0.25F, 0.5F, 0.75F // u0, u1, v0, v1
        };
        int argb = 0xCC112233;
        int light = 0x00F00080;
        int[] ints = new int[]{argb, light};
        int[] dst = new int[ParticleFastTrack.INSTANCE_INTS];
        ParticleFastTrack.packParticles(floats, ints, 1, dst, 0);

        // Verify packed layout fields.
        assertEquals(1.5F, Float.intBitsToFloat(dst[0]));
        assertEquals(-2.25F, Float.intBitsToFloat(dst[1]));
        assertEquals(3.75F, Float.intBitsToFloat(dst[2]));
        assertEquals(0.75F, Float.intBitsToFloat(dst[3]));
        assertEquals(0.3F, Float.intBitsToFloat(dst[4]));
        assertEquals(-0.4F, Float.intBitsToFloat(dst[5]));
        assertEquals(0.5F, Float.intBitsToFloat(dst[6]));
        assertEquals(1.2F, Float.intBitsToFloat(dst[7]));
        assertEquals(0.125F, Float.intBitsToFloat(dst[8]));
        assertEquals(0.25F, Float.intBitsToFloat(dst[9]));
        assertEquals(0.5F, Float.intBitsToFloat(dst[10]));
        assertEquals(0.75F, Float.intBitsToFloat(dst[11]));
        assertEquals(argb, dst[12]);
        assertEquals(light, dst[13]);

        // Verify InstColor.bgra matches ARGB.toABGR byte channels.
        int abgr = ARGB.toABGR(argb);
        int raw = dst[12];
        // Little-endian raw bytes: byte0=B, byte1=G, byte2=R, byte3=A -> .bgra = (byte2=R, byte1=G, byte0=B, byte3=A)
        assertEquals(abgr & 0xFF, (raw >>> 16) & 0xFF);
        assertEquals((abgr >>> 8) & 0xFF, (raw >>> 8) & 0xFF);
        assertEquals((abgr >>> 16) & 0xFF, raw & 0xFF);
        assertEquals((abgr >>> 24) & 0xFF, (raw >>> 24) & 0xFF);

        // Verify instanced shader quaternion math (including rnK) matches JOML Vector3f.rotate(Quaternionf).
        float[][] corners = {{1, -1, 1, 1}, {1, 1, 1, 0}, {-1, 1, 0, 0}, {-1, -1, 0, 1}};
        Quaternionf q = new Quaternionf(floats[3], floats[4], floats[5], floats[6]);
        float rx = Float.intBitsToFloat(dst[4]), ry = Float.intBitsToFloat(dst[5]),
              rz = Float.intBitsToFloat(dst[6]), rw = Float.intBitsToFloat(dst[7]);
        float rnXX = rx * rx, rnYY = ry * ry, rnZZ = rz * rz, rnWW = rw * rw;
        float rnK = 1.0F / (rnXX + rnYY + rnZZ + rnWW);
        float rnXY = rx * ry, rnXZ = rx * rz, rnYZ = ry * rz;
        float rnXW = rx * rw, rnYW = ry * rw, rnZW = rz * rw;

        for (float[] corner : corners) {
            Vector3f vanilla = new Vector3f(corner[0], corner[1], 0.0F).rotate(q).mul(floats[7]).add(floats[0], floats[1], floats[2]);
            float gx = ((rnWW + rnXX - rnZZ - rnYY) * rnK * corner[0] + (2.0F * (rnXY - rnZW) * rnK) * corner[1]) * floats[7] + floats[0];
            float gy = ((2.0F * (rnXY + rnZW) * rnK) * corner[0] + (rnYY - rnZZ + rnWW - rnXX) * rnK * corner[1]) * floats[7] + floats[1];
            float gz = ((2.0F * (rnXZ - rnYW) * rnK) * corner[0] + (2.0F * (rnYZ + rnXW) * rnK) * corner[1]) * floats[7] + floats[2];
            assertEquals(vanilla.x, gx, 1e-5F);
            assertEquals(vanilla.y, gy, 1e-5F);
            assertEquals(vanilla.z, gz, 1e-5F);
        }

        // Verify direct off-heap MemorySegment packing (P2.2) matches packParticles bit-for-bit.
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment seg = arena.allocate(ParticleFastTrack.INSTANCE_BYTES, 16);
            ParticleFastTrack.packParticlesToSegment(floats, ints, 1, seg, 0);
            for (int i = 0; i < ParticleFastTrack.INSTANCE_INTS; ++i) {
                assertEquals(dst[i], seg.get(ValueLayout.JAVA_INT, i * 4L), "Off-heap particle int mismatch at " + i);
            }
        }
    }

    @Test
    void fastBucketGroupingReusesPooledRunTableAcrossFrames() {
        ModelFastTrack.FastBucket bucket = ModelFastTrack.acquireFastBucket(null, true);
        PoseStack pose = new PoseStack();
        Vector3f scratch = new Vector3f();
        // Frame 1: interleaved meshIds [10, 20, 10, 30, 20, 10] -> grouped runs: (10, count=3), (20, count=2), (30, count=1).
        int[] ids1 = {10, 20, 10, 30, 20, 10};
        for (int i = 0; i < ids1.length; ++i) {
            bucket.addInstance(null, ids1[i], 0, pose.last(), 0xFF000000 | i, i, i + 100, scratch);
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment dst = arena.allocate((long) ids1.length * RetainedModelManager.INSTANCE_BYTES, 16);
            bucket.buildGroupedAndCopy(dst);
            assertEquals(3, bucket.runCount());
            assertEquals(10, bucket.runMeshId(0)); assertEquals(0, bucket.runFirstInstance(0)); assertEquals(3, bucket.runInstanceCount(0));
            assertEquals(20, bucket.runMeshId(1)); assertEquals(3, bucket.runFirstInstance(1)); assertEquals(2, bucket.runInstanceCount(1));
            assertEquals(30, bucket.runMeshId(2)); assertEquals(5, bucket.runFirstInstance(2)); assertEquals(1, bucket.runInstanceCount(2));
            // Verify overlay field (int offset 13) of grouped instances preserved original order per meshId.
            assertEquals(0, dst.get(ValueLayout.JAVA_INT, 0L * RetainedModelManager.INSTANCE_BYTES + 13 * 4L));
            assertEquals(2, dst.get(ValueLayout.JAVA_INT, 1L * RetainedModelManager.INSTANCE_BYTES + 13 * 4L));
            assertEquals(5, dst.get(ValueLayout.JAVA_INT, 2L * RetainedModelManager.INSTANCE_BYTES + 13 * 4L));
            assertEquals(1, dst.get(ValueLayout.JAVA_INT, 3L * RetainedModelManager.INSTANCE_BYTES + 13 * 4L));
            assertEquals(4, dst.get(ValueLayout.JAVA_INT, 4L * RetainedModelManager.INSTANCE_BYTES + 13 * 4L));
            assertEquals(3, dst.get(ValueLayout.JAVA_INT, 5L * RetainedModelManager.INSTANCE_BYTES + 13 * 4L));
        }
        ModelFastTrack.recycleFastBucket(bucket);

        // Frame 2: acquire pooled bucket again and verify clean generation-stamped reset with overlapping/new meshIds.
        ModelFastTrack.FastBucket reused = ModelFastTrack.acquireFastBucket(null, true);
        assertSame(bucket, reused);
        int[] ids2 = {20, 40, 20};
        for (int i = 0; i < ids2.length; ++i) {
            reused.addInstance(null, ids2[i], 0, pose.last(), 0xFF000000 | (i + 10), i + 10, 0, scratch);
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment dst = arena.allocate((long) ids2.length * RetainedModelManager.INSTANCE_BYTES, 16);
            reused.buildGroupedAndCopy(dst);
            assertEquals(2, reused.runCount());
            assertEquals(20, reused.runMeshId(0)); assertEquals(0, reused.runFirstInstance(0)); assertEquals(2, reused.runInstanceCount(0));
            assertEquals(40, reused.runMeshId(1)); assertEquals(2, reused.runFirstInstance(1)); assertEquals(1, reused.runInstanceCount(1));
        }
        ModelFastTrack.recycleFastBucket(reused);
    }

    @Test
    void translatedLeafPackingMatchesVanillaPoseStackTranslateBitForBit() {
        Random rng = new Random(0x726179L);
        int[] expected = new int[RetainedModelManager.INSTANCE_INTS];
        int[] actual = new int[RetainedModelManager.INSTANCE_INTS];
        Vector3f scratch = new Vector3f();
        for (int i = 0; i < 200; ++i) {
            PoseStack stack = new PoseStack();
            stack.translate((rng.nextFloat() - 0.5F) * 10.0F, (rng.nextFloat() - 0.5F) * 10.0F, (rng.nextFloat() - 0.5F) * 10.0F);
            stack.rotate(new Quaternionf().rotationZYX(rng.nextFloat() * 6.28F, rng.nextFloat() * 6.28F, rng.nextFloat() * 6.28F));
            if ((i & 1) != 0) stack.scale(0.5F + rng.nextFloat() * 2.0F, -1.0F, 0.75F + rng.nextFloat());
            float tx = (rng.nextFloat() - 0.5F) * 4.0F;
            float ty = (rng.nextFloat() - 0.5F) * 4.0F;
            float tz = (rng.nextFloat() - 0.5F) * 4.0F;
            RetainedModelManager.packInstanceTranslated(stack.last(), tx, ty, tz, 0xFF123456, 10, 0xF000F0, actual, 0, scratch);
            stack.pushPose();
            stack.translate(tx, ty, tz);
            RetainedModelManager.packInstance(stack.last(), 0xFF123456, 10, 0xF000F0, expected, 0, scratch);
            stack.popPose();
            assertArrayEquals(expected, actual, "Translated leaf packing diverged at trial " + i);
        }
    }

    @Test
    void shadowQuadMatchesTransformedVanillaCornersAndUvs() {
        var pose = new Matrix4f().translation(123, -21, 1).rotateZYX(0.7f, 0.4f, -1.1f).scale(-2, 3, 0.5f);
        try (var arena = Arena.ofConfined()) {
            var memory = arena.allocate(56, 8);
            int color = ARGB.white(0.375f);
            ShadowFastTrack.pack(memory, 0, pose, 2, -1, 3, 0.25f, 2, 7, color);
            for (int x = 0; x <= 1; ++x) for (int z = 0; z <= 1; ++z) {
                float px = x == 0 ? -1 : 3, pz = z == 0 ? 2 : 7;
                var expected = pose.transformPosition(px, 0.25f, pz, new Vector3f());
                for (int axis = 0; axis < 3; ++axis) {
                    long at = axis * 4L;
                    float actual = memory.get(ValueLayout.JAVA_FLOAT, at) +
                        x * memory.get(ValueLayout.JAVA_FLOAT, 16 + at) + z * memory.get(ValueLayout.JAVA_FLOAT, 32 + at);
                    assertEquals(expected.get(axis), actual, 2e-5f);
                }
                assertEquals(-px / 4 + 0.5f, memory.get(ValueLayout.JAVA_FLOAT, x == 0 ? 12 : 28));
                assertEquals(-pz / 4 + 0.5f, memory.get(ValueLayout.JAVA_FLOAT, z == 0 ? 44 : 48));
            }
            assertEquals(color, memory.get(ValueLayout.JAVA_INT, 52));
        }
    }

    @Test
    void promotionSkipsFirstUseInEachEpoch() {
        class Handle implements RetainedModelManager.PartHandle {
            long epoch;
            int id,page;
            public long raynium$getMeshEpoch() { return epoch; }
            public int raynium$getMeshId() { return id; }
            public int raynium$getMeshPage() { return page; }
            public void raynium$setMeshCache(long e,int i,int p) { epoch=e; id=i; page=p; }
            public List<ModelPart.Cube> raynium$getCubes() { throw new AssertionError(); }
        }
        var handle=new Handle();
        assertEquals(-1,RetainedModelManager.promotionState(handle,1));
        assertEquals(-2,RetainedModelManager.promotionState(handle,1));
        handle.raynium$setMeshCache(1,16,3);
        assertEquals(16,RetainedModelManager.promotionState(handle,1));
        assertEquals(-1,RetainedModelManager.promotionState(handle,2));
    }

    @Test
    void interleavedPagesProduceContiguousRunsAndInstanceBytes() {
        var bucket=ModelFastTrack.acquireFastBucket(null,true);
        var pose=new PoseStack().last();
        var scratch=new Vector3f();
        int[] ids={16,17,18,19,17};
        for(int i=0;i<ids.length;i++) bucket.addInstance(null,ids[i],ids[i]%2,pose,-1,i,0,scratch);
        try(var arena=Arena.ofConfined()) {
            var dst=arena.allocate(80L*ids.length,8);
            bucket.buildGroupedAndCopy(dst);
            int[] expected={16,18,17,19};
            int[] order={0,2,1,4,3};
            for(int i=0;i<expected.length;i++) assertEquals(expected[i],bucket.runMeshId(i));
            for(int i=0;i<order.length;i++) assertEquals(order[i],dst.get(ValueLayout.JAVA_INT,80L*i+52));
        } finally { ModelFastTrack.recycleFastBucket(bucket); }
    }

    @Test
    void equivalentTransientRenderTypesHitTheSameBucket() {
        var setup=net.minecraft.client.renderer.rendertype.RenderSetup.builder(
            net.minecraft.client.renderer.RenderPipelines.ENTITY_SOLID).createRenderSetup();
        var ext=ModelFastTrack.acquireGroupExtension(true,0);
        var groups=new ModelFastTrack.FastBucket[32];
        try {
            for(int i=0;i<1000;i++) {
                int key=i%groups.length;
                var rt=net.minecraft.client.renderer.rendertype.RenderType.create("dynamic",setup);
                var prepared=new net.minecraft.client.renderer.rendertype.PreparedRenderType("material"+key,
                    rt.pipeline(),null,null,null,List.of());
                var bucket=ext.acquireFastBucket(rt,prepared);
                if(groups[key]==null) groups[key]=bucket;
                else assertSame(groups[key],bucket);
            }
        } finally { ext.recycle(); }
    }

    private static float unpackSnorm8(int packed, int lane) {
        byte signed = (byte) ((packed >>> (lane * 8)) & 0xFF);
        return Math.max(-1.0F, signed / 127.0F);
    }
}
