package dev.raynium.bridge;

import com.mojang.blaze3d.buffers.Std140Builder;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.world.ParticleFastTrack;
import dev.raynium.client.world.RetainedModelManager;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.Executors;
import net.minecraft.client.renderer.RenderPipelines;
import org.joml.Matrix4f;
import org.joml.Vector3f;
import org.joml.Vector4f;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeWorldFeaturesTest {
    @TempDir Path directory;

    private static GpuBuffer makeUbo(RayniumGpuDevice device, int bytes, java.util.function.Consumer<Std140Builder> writer) {
        ByteBuffer buf = ByteBuffer.allocateDirect(bytes).order(ByteOrder.nativeOrder());
        writer.accept(Std140Builder.intoBuffer(buf));
        buf.flip();
        return device.createBuffer(() -> "test-ubo", GpuBuffer.USAGE_UNIFORM | GpuBuffer.USAGE_COPY_DST, buf);
    }

    private static byte[] makeUnitQuadModelMesh() {
        // 4 vertices * 24 bytes: covers [-0.5, +0.5] x [-0.5, +0.5] at z = 0, facing +Z.
        ByteBuffer buf = ByteBuffer.allocate(4 * 24).order(ByteOrder.LITTLE_ENDIAN);
        float[][] verts = {
            {-0.5F, -0.5F, 0.0F, 0.0F, 1.0F},
            { 0.5F, -0.5F, 0.0F, 1.0F, 1.0F},
            { 0.5F,  0.5F, 0.0F, 1.0F, 0.0F},
            {-0.5F,  0.5F, 0.0F, 0.0F, 0.0F}
        };
        for (float[] v : verts) {
            buf.putFloat(v[0]).putFloat(v[1]).putFloat(v[2]).putFloat(v[3]).putFloat(v[4]);
            buf.put((byte) 0).put((byte) 0).put((byte) 127).put((byte) 0);
        }
        return buf.array();
    }

    @ParameterizedTest
    @EnumSource(NativeEnvironment.Backend.class)
    void retainedModelsAndCompactParticlesRenderAndValidateAcrossBackends(NativeEnvironment.Backend api) throws Exception {
        List<String> logs = new ArrayList<>();
        try (var env = new NativeEnvironment(NativeTestSupport.locate(directory), api, NativeTestSupport.recordingLogger(logs));
             var runtime = env.createRuntime();
             var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var device = new RayniumGpuDevice(runtime, renderer, true, directory);
             var sources = new NativeRenderPearlTest.Sources();
             var executor = Executors.newSingleThreadExecutor()) {

            // ABI 22 validation for rn_upsert_model_mesh / rn_remove_model_mesh.
            byte[] quadMesh = makeUnitQuadModelMesh();
            assertThrows(IllegalArgumentException.class, () -> renderer.upsertModelMesh(0, quadMesh, 4, 6));
            assertThrows(IllegalArgumentException.class, () -> renderer.removeModelMesh(-1));
            assertThrows(IllegalStateException.class, () -> renderer.upsertModelMesh(16, quadMesh, 4, 5)); // wrong index count
            assertThrows(IllegalStateException.class, () -> renderer.upsertModelMesh(16, new byte[24], 1, 6)); // non-quad vertex count

            // Upsert, replace, and remove cleanly.
            assertEquals(0, renderer.upsertModelMesh(16, quadMesh, 4, 6));
            assertEquals(0, renderer.upsertModelMesh(16, quadMesh, 4, 6));
            renderer.upsertModelMesh(17, quadMesh, 4, 6);
            renderer.removeModelMesh(17);
            renderer.upsertModelMesh(18, quadMesh, 4, 6);

            // Compile vanilla ENTITY_SOLID and OPAQUE_PARTICLE and confirm built-in instanced pipelines registered.
            var entityPipeline = Objects.requireNonNull(
                device.compilePipeline(RenderPipelines.ENTITY_SOLID, sources, executor).join().finishCompile()
            );
            var particlePipeline = Objects.requireNonNull(
                device.compilePipeline(RenderPipelines.OPAQUE_PARTICLE, sources, executor).join().finishCompile()
            );
            assertTrue(device.supportsInstanced(RenderPipelines.ENTITY_SOLID, "entity"));
            assertTrue(device.supportsInstanced(RenderPipelines.OPAQUE_PARTICLE, "particle"));

            var shadowPipeline = Objects.requireNonNull(device.compilePipeline(RenderPipelines.ENTITY_SHADOW, sources, executor).join().finishCompile());
            assertTrue(device.supportsInstanced(RenderPipelines.ENTITY_SHADOW, "shadow"));
            try (entityPipeline; particlePipeline; shadowPipeline;
                 var colorTex = device.createTexture("world-color", GpuTexture.USAGE_RENDER_ATTACHMENT | GpuTexture.USAGE_COPY_SRC, GpuFormat.RGBA8_UNORM, 4, 4, 1, 1);
                 var depthTex = device.createTexture("world-depth", GpuTexture.USAGE_RENDER_ATTACHMENT, GpuFormat.D32_FLOAT, 4, 4, 1, 1);
                 var whiteTex = device.createTexture("white", GpuTexture.USAGE_COPY_DST | GpuTexture.USAGE_TEXTURE_BINDING, GpuFormat.RGBA8_UNORM, 1, 1, 1, 1);
                 var colorView = device.createTextureView(colorTex);
                 var depthView = device.createTextureView(depthTex);
                 var whiteView = device.createTextureView(whiteTex);
                 var sampler = device.createSampler(AddressMode.CLAMP_TO_EDGE, AddressMode.CLAMP_TO_EDGE, FilterMode.NEAREST, FilterMode.NEAREST, 1, OptionalDouble.empty());
                 var projectionUbo = makeUbo(device, 64, b -> b.putMat4f(new Matrix4f().identity()));
                 var dynamicUbo = makeUbo(device, 192, b -> b
                     .putMat4f(new Matrix4f().identity())
                     .putMat4f(new Matrix4f().identity())
                     .putVec4(1, 1, 1, 1)
                     .putVec3(0, 0, 0));
                 var fogUbo = makeUbo(device, 48, b -> b
                     .putVec4(0, 0, 0, 0)
                     .putFloat(1000).putFloat(2000).putFloat(1000).putFloat(2000).putFloat(1000).putFloat(2000));
                 var lightingUbo = makeUbo(device, 32, b -> b.putVec3(0, 0, 1).putVec3(0, 0, 1));
                 var readback = device.createBuffer(() -> "readback", GpuBuffer.USAGE_COPY_DST | GpuBuffer.USAGE_MAP_READ, 4 * 4 * 4)) {

                var encoder = device.createCommandEncoder();
                ByteBuffer whitePixel = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder()).putInt(0xFFFFFFFF).flip();
                encoder.writeToTexture(whiteTex, whitePixel, 0, 0, 0, 0, 1, 1);

                try (RenderPass pass = encoder.createRenderPass(() -> "mixed-world-pass",
                        colorView, Optional.of(new Vector4f(0, 0, 0, 1)), depthView, OptionalDouble.of(0.0))) {
                    CommandTape tape = RayniumGpuDevice.activeTape(pass);

                    // The model instances cover the top two rows.
                    // Left quadrant (x in [-1, 0]): pure red; Right quadrant (x in [0, +1]): pure green.
                    int[] modelInts = new int[2 * RetainedModelManager.INSTANCE_INTS];
                    Vector3f scratch = new Vector3f();
                    PoseStack leftPose = new PoseStack();
                    leftPose.translate(-0.5F, -0.5F, 0.5F);
                    RetainedModelManager.packInstance(leftPose.last(), 0xFFFF0000, 0, 0x00F000F0, modelInts, 0, scratch);

                    PoseStack rightPose = new PoseStack();
                    rightPose.translate(0.5F, -0.5F, 0.5F);
                    RetainedModelManager.packInstance(rightPose.last(), 0xFF00FF00, 0, 0x00F000F0, modelInts, RetainedModelManager.INSTANCE_INTS, scratch);

                    long modelOffset = tape.reserveInstanceUpload(2L * RetainedModelManager.INSTANCE_BYTES, 8);
                    MemorySegment.copy(modelInts, 0, tape.instanceUploadSlice(modelOffset, 2L * RetainedModelManager.INSTANCE_BYTES), ValueLayout.JAVA_INT, 0, modelInts.length);

                    pass.setPipeline(entityPipeline);
                    pass.setUniform("Projection", projectionUbo.slice());
                    pass.setUniform("DynamicTransforms", dynamicUbo.slice());
                    pass.setUniform("Fog", fogUbo.slice());
                    pass.setUniform("Lighting", lightingUbo.slice());
                    pass.setUniform("Sampler0", whiteView, sampler);
                    pass.setUniform("Sampler1", whiteView, sampler);
                    pass.setUniform("Sampler2", whiteView, sampler);
                    // Multi-run same-page batch (meshId 1 + meshId 3) exercises same-page indirect draw batching (P2.4).
                    assertTrue(RayniumGpuDevice.recordInstancedGeometry(pass, "entity", modelOffset, 2, 80, new int[]{16, 18}, new int[]{0, 1}, new int[]{1, 1}, 2));
                    // Re-submit same uploadOffset to exercise zero-copy BorrowedBytes OIT slice reuse (P2.1).
                    assertTrue(RayniumGpuDevice.recordInstancedGeometry(pass, "entity", modelOffset, 2, 80, new int[]{16, 18}, new int[]{0, 1}, new int[]{1, 1}, 2));

                }
                try (var transfer = device.createBuffer(() -> "interleaved upload", GpuBuffer.USAGE_COPY_DST, 2 << 20)) {
                    encoder.writeToBuffer(transfer.slice(), ByteBuffer.allocateDirect(2 << 20));
                    encoder.writeToTexture(whiteTex, whitePixel.duplicate(), 0, 0, 0, 0, 1, 1);
                }
                try (RenderPass pass = encoder.createRenderPass(() -> "particles",
                        colorView, Optional.empty(), depthView, OptionalDouble.empty())) {
                    CommandTape tape = RayniumGpuDevice.activeTape(pass);
                    // The particle instances cover the bottom two rows.
                    // Bottom-left: pure blue (ARGB 0xFF0000FF); Bottom-right: pure yellow (ARGB 0xFFFFFF00).
                    float[] particleFloats = new float[]{
                        -0.5F,  0.5F, 0.5F,  0, 0, 0, 1,  0.5F,  0, 1, 0, 1,
                         0.5F,  0.5F, 0.5F,  0, 0, 0, 1,  0.5F,  0, 1, 0, 1
                    };
                    int[] particleColorsLight = new int[]{
                        0xFF0000FF, 0x00F000F0,
                        0xFFFFFF00, 0x00F000F0
                    };
                    int[] particleInts = new int[2 * ParticleFastTrack.INSTANCE_INTS];
                    ParticleFastTrack.packParticles(particleFloats, particleColorsLight, 2, particleInts, 0);
                    long particleOffset = tape.reserveInstanceUpload(2L * ParticleFastTrack.INSTANCE_BYTES, 8);
                    assertEquals(2L * RetainedModelManager.INSTANCE_BYTES, particleOffset);
                    MemorySegment.copy(particleInts, 0, tape.instanceUploadSlice(particleOffset, 2L * ParticleFastTrack.INSTANCE_BYTES), ValueLayout.JAVA_INT, 0, particleInts.length);

                    pass.setPipeline(particlePipeline);
                    pass.setUniform("Projection", projectionUbo.slice());
                    pass.setUniform("DynamicTransforms", dynamicUbo.slice());
                    pass.setUniform("Fog", fogUbo.slice());
                    pass.setUniform("Sampler0", whiteView, sampler);
                    pass.setUniform("Sampler2", whiteView, sampler);
                    assertTrue(RayniumGpuDevice.recordInstancedGeometry(pass, "particle", particleOffset, 2, 56, new int[]{1}, new int[]{0}, new int[]{2}, 1));
                    assertTrue(RayniumGpuDevice.recordInstancedGeometry(pass, "particle", particleOffset, 2, 56, new int[]{1}, new int[]{0}, new int[]{2}, 1));
                }

                encoder.copyTextureToBuffer(colorTex, readback, 0, () -> {}, 0);
                try (var fence = encoder.createFence()) {
                    encoder.submit();
                    assertTrue(fence.awaitCompletion(-1));
                }

                try (var mapped = readback.slice().map(true, false)) {
                    // Check all 4 quadrants of the 4x4 image (top-left=red, top-right=green, bottom-left=blue, bottom-right=yellow).
                    int topLeft = mapped.data().getInt(0);
                    int topRight = mapped.data().getInt(3 * 4);
                    int botLeft = mapped.data().getInt(3 * 16);
                    int botRight = mapped.data().getInt(3 * 16 + 3 * 4);
                    // RGBA8 little-endian: byte0=R, byte1=G, byte2=B, byte3=A.
                    // ENTITY_SOLID applies diffuse lighting (strictly >0 on red/green channels) and white texture.
                    assertTrue((topLeft & 0xFF) > 100 && ((topLeft >>> 8) & 0xFF) == 0 && ((topLeft >>> 16) & 0xFF) == 0,
                        "Expected red model in top-left, got 0x" + Integer.toHexString(topLeft));
                    assertTrue((topRight & 0xFF) == 0 && ((topRight >>> 8) & 0xFF) > 100 && ((topRight >>> 16) & 0xFF) == 0,
                        "Expected green model in top-right, got 0x" + Integer.toHexString(topRight));
                    // Particle is unlit (pure blue = 0xFFFF0000 in ABGR, pure yellow = 0xFF00FFFF in ABGR).
                    assertEquals(0xFFFF0000, botLeft, "Expected blue particle in bottom-left");
                    assertEquals(0xFF00FFFF, botRight, "Expected yellow particle in bottom-right");
                }
                try (RenderPass pass = encoder.createRenderPass(() -> "shadow", colorView,
                        Optional.of(new Vector4f(0, 0, 0, 1)), depthView, OptionalDouble.of(0.0))) {
                    var tape=RayniumGpuDevice.activeTape(pass);
                    long offset=tape.reserveInstanceUpload(56,8);
                    var bytes=tape.instanceUploadSlice(offset,56);
                    float[] corners={-1,1,0.5f,0, 2,0,0,1, 0,-2,0,0, 1};
                    MemorySegment.copy(corners,0,bytes,ValueLayout.JAVA_FLOAT,0,corners.length);
                    bytes.set(ValueLayout.JAVA_INT,52,0x80ffffff);
                    pass.setPipeline(shadowPipeline);
                    pass.setUniform("DynamicTransforms",dynamicUbo.slice());
                    pass.setUniform("Projection",projectionUbo.slice());
                    pass.setUniform("Fog",fogUbo.slice());
                    pass.setUniform("Sampler0",whiteView,sampler);
                    assertTrue(RayniumGpuDevice.recordInstancedGeometry(pass,"shadow",offset,1,56,
                        new int[]{2},new int[]{0},new int[]{1},1));
                }
                encoder.copyTextureToBuffer(colorTex,readback,0,() -> {},0);
                try(var fence=encoder.createFence()) {
                    encoder.submit();
                    assertTrue(fence.awaitCompletion(-1));
                }
                try(var mapped=readback.slice().map(true,false)) {
                    for(int pixel=0;pixel<16;pixel++)
                        assertEquals(0x808080,mapped.data().getInt(pixel*4)&0xffffff,"Blended shadow pixel "+pixel);
                }
            }
            assertTrue(logs.stream().noneMatch(line -> line.contains("VALIDATION") || line.contains("ERROR")),
                "Unexpected GPU validation errors: " + logs);
        }
    }
}
