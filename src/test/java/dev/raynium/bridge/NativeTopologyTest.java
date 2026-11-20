package dev.raynium.bridge;

import com.mojang.blaze3d.vertex.BufferBuilder;
import com.mojang.blaze3d.vertex.ByteBufferBuilder;
import com.mojang.blaze3d.vertex.DefaultVertexFormat;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.RenderPassDescriptor;
import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Optional;
import java.util.OptionalDouble;
import java.util.concurrent.Executors;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.resources.Identifier;
import org.joml.Vector4f;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeTopologyTest {
    @TempDir Path directory;
    private static final int SIZE = 16;

    private static ByteBuffer bytes(int size) {
        return ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
    }

    private static ByteBuffer vertices(float... coordinates) {
        var data = bytes(coordinates.length / 2 * 16);
        for (int i = 0; i < coordinates.length; i += 2) {
            data.putFloat(coordinates[i]).putFloat(coordinates[i + 1]).putFloat(.5f).putInt(-1);
        }
        return data.flip();
    }

    private static ByteBuffer sequential(int... indices) {
        var data = bytes(indices.length * 2);
        for (int index : indices) data.putShort((short) index);
        return data.flip();
    }

    private static ByteBuffer wideLine() {
        try (var storage = new ByteBufferBuilder(256)) {
            var builder = new BufferBuilder(storage, PrimitiveTopology.LINES, DefaultVertexFormat.POSITION_COLOR_NORMAL_LINE_WIDTH);
            for (float x : new float[]{-.75f, .75f}) {
                builder.addVertex(x, 0, .5f).setColor(-1).setNormal(1, 0, 0).setLineWidth(4);
            }
            try (var mesh = builder.buildOrThrow()) {
                assertEquals(4, mesh.drawState().vertexCount());
                assertEquals(6, mesh.drawState().indexCount());
                var data = mesh.vertexBuffer();
                return bytes(data.remaining()).put(data).flip();
            }
        }
    }

    private static ByteBuffer points() {
        var data = bytes(40);
        for (float x : new float[]{-.5f, .5f}) {
            data.putFloat(x).putFloat(0).putFloat(.5f).putInt(-1).putFloat(4);
        }
        return data.flip();
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rendersTriangleFansLinesStripsAndPixelSizedPoints(NativeEnvironment.Backend api) throws Exception {
        try (var env = new NativeEnvironment(NativeTestSupport.locate(directory), api, NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime();
             var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var device = new RayniumGpuDevice(runtime, renderer, true);
             var source = new NativeRenderPearlTest.Sources();
             var executor = Executors.newSingleThreadExecutor();
             var transforms = device.createBuffer(null, GpuBuffer.USAGE_UNIFORM, NativeRenderPearlTest.constants(true));
             var globals = device.createBuffer(null, GpuBuffer.USAGE_UNIFORM, bytes(256).putFloat(32, SIZE).putFloat(36, SIZE));
             var fog = device.createBuffer(null, GpuBuffer.USAGE_UNIFORM, bytes(256));
             var target = device.createTexture("topology", GpuTexture.USAGE_RENDER_ATTACHMENT | GpuTexture.USAGE_COPY_SRC, GpuFormat.RGBA8_UNORM, SIZE, SIZE, 1, 1);
             var depth = device.createTexture("depth", GpuTexture.USAGE_RENDER_ATTACHMENT, GpuFormat.D32_FLOAT, SIZE, SIZE, 1, 1);
             var view = device.createTextureView(target);
             var z = device.createTextureView(depth);
             var read = device.createBuffer(null, GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, SIZE * SIZE * 4)) {
            var encoder = device.createCommandEncoder();
            var triangles = vertices(-1,-1, -.25f,-1, -1,-.25f, 0,-1, .75f,-1, 0,-.25f,
                                     -1,0, -.25f,0, -1,.75f, 0,0, .75f,0, 0,.75f);
            var strip = vertices(-.5f,-.5f, .5f,-.5f, -.5f,.5f, .5f,.5f);
            var fan = vertices(2,2, -.5f,-.5f, .5f,-.5f, .5f,.5f, -.5f,.5f);
            var line = vertices(-.75f,.0625f, .75f,.0625f, .75f,.8125f);
            record Draw(PrimitiveTopology mode, ByteBuffer vertices, ByteBuffer indices, int count, int first, int base, boolean wide) {}
            var draws = new Draw[]{
                new Draw(PrimitiveTopology.TRIANGLES, triangles, null, 12, 0, 0, false),
                new Draw(PrimitiveTopology.QUADS, triangles, null, 12, 0, 0, false),
                new Draw(PrimitiveTopology.LINES, triangles, null, 12, 0, 0, false),
                new Draw(PrimitiveTopology.TRIANGLE_STRIP, strip, null, 4, 0, 0, false),
                new Draw(PrimitiveTopology.TRIANGLE_FAN, fan, null, 4, 1, 0, false),
                new Draw(PrimitiveTopology.TRIANGLE_FAN, fan, sequential(0,1,2,3), 4, 0, 1, false),
                new Draw(PrimitiveTopology.DEBUG_LINES, line, null, 2, 0, 0, false),
                new Draw(PrimitiveTopology.DEBUG_LINE_STRIP, line, null, 3, 0, 0, false),
                new Draw(PrimitiveTopology.POINTS, points(), sequential(0), 1, 0, 1, false),
                new Draw(PrimitiveTopology.LINES, wideLine(), sequential(0,1,2,3,2,1), 6, 0, 0, true)
            };
            boolean[] expectedTriangles = null;
            for (var draw : draws) {
                var pipeline = draw.wide ? RenderPipelines.LINES : draw.mode == PrimitiveTopology.POINTS ? RenderPipelines.DEBUG_POINTS :
                    RenderPipeline.builder(RenderPipelines.GUI_SNIPPET).withLocation(Identifier.parse("raynium:topology"))
                        .withCull(false).withPrimitiveTopology(draw.mode).build();
                try (var compiled = device.compilePipeline(pipeline, source, executor).join().finishCompile();
                     var vb = device.createBuffer(null, GpuBuffer.USAGE_VERTEX, draw.vertices.duplicate());
                     var ib = draw.indices == null ? null : device.createBuffer(null, GpuBuffer.USAGE_INDEX, draw.indices)) {
                    var desc = RenderPassDescriptor.builder(() -> draw.mode.name()).withColorAttachment(view, Optional.of(new Vector4f(0,0,0,1)))
                        .withDepthAttachment(z, OptionalDouble.of(0)).build();
                    try (var pass = encoder.createRenderPass(desc)) {
                        pass.setPipeline(compiled);
                        pass.setVertexBuffer(0, vb.slice());
                        pass.setUniform("DynamicTransforms", transforms);
                        pass.setUniform("Projection", transforms);
                        if (draw.wide || draw.mode == PrimitiveTopology.POINTS) pass.setUniform("Globals", globals);
                        if (draw.wide) pass.setUniform("Fog", fog);
                        if (ib != null) {
                            pass.setIndexBuffer(ib, IndexType.SHORT);
                            pass.drawIndexed(draw.count, 1, draw.first, draw.base, 0);
                        } else pass.draw(draw.count, 1, draw.first, 0);
                    }
                    encoder.copyTextureToBuffer(target, read, 0, () -> {}, 0);
                    try (var done = encoder.createFence()) { encoder.submit(); assertTrue(done.awaitCompletion(-1)); }
                    try (var mapped = read.slice().map(true, false)) {
                        boolean[] hits = new boolean[SIZE * SIZE];
                        int count = 0;
                        for (int i = 0; i < hits.length; ++i) {
                            int pixel = mapped.data().getInt(4*i);
                            assertTrue(pixel == 0xff000000 || pixel == 0xff00ff00, draw.mode.name());
                            hits[i] = pixel == 0xff00ff00;
                            if (hits[i]) ++count;
                        }
                        if (draw.wide) { assertEquals(48, count); assertTrue(hits[8*SIZE+8]); }
                        else switch (draw.mode) {
                            case TRIANGLES -> {
                                for (int block = 0; block < 4; ++block) {
                                    int covered = 0;
                                    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
                                        if (hits[(8*(block/2)+y)*SIZE+8*(block%2)+x]) ++covered;
                                    assertTrue(covered == 15 || covered == 21, "triangle coverage "+covered);
                                }
                                expectedTriangles = hits;
                            }
                            case QUADS, LINES -> assertArrayEquals(expectedTriangles, hits, draw.mode.name());
                            case TRIANGLE_STRIP, TRIANGLE_FAN, POINTS -> {
                                boolean point = draw.mode == PrimitiveTopology.POINTS;
                                for (int y = 0; y < SIZE; ++y) for (int x = 0; x < SIZE; ++x)
                                    assertEquals(point ? x >= 10 && x < 14 && y >= 6 && y < 10 : x >= 4 && x < 12 && y >= 4 && y < 12,
                                        hits[y*SIZE+x], draw.mode+" at "+x+","+y);
                            }
                            case DEBUG_LINES -> assertEquals(12, count);
                            case DEBUG_LINE_STRIP -> assertTrue(count >= 17 && count <= 19, "line strip coverage "+count);
                        }
                    }
                }
            }
        }
    }
}
