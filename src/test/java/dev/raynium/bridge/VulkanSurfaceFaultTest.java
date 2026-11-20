package dev.raynium.bridge;

import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.device.GpuDeviceLossException;
import com.mojang.renderpearl.api.device.GpuSurface;
import com.mojang.renderpearl.api.device.SurfaceException;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.renderpearl.TerrainTestPass;
import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
@Tag("vulkan-surface-fault")
class VulkanSurfaceFaultTest {
    @TempDir Path directory;
    private static final int LOST = -4, SUBOPTIMAL = 1000001003, OUT_OF_DATE = -1000001004, SURFACE_LOST = -1000000000;
    private static final GpuSurface.Configuration CONFIG = new GpuSurface.Configuration(640, 360, GpuSurface.PresentMode.IMMEDIATE);
    private static final int TEXTURE_USAGE = GpuTexture.USAGE_RENDER_ATTACHMENT | GpuTexture.USAGE_TEXTURE_BINDING
        | GpuTexture.USAGE_COPY_SRC | GpuTexture.USAGE_COPY_DST;

    private record Probe(MethodHandle arm, MethodHandle counter) {
        static Probe load(Arena arena) {
            var library = SymbolLookup.libraryLookup(Path.of(System.getProperty("raynium.test.surfaceLayer")), arena);
            var linker = Linker.nativeLinker();
            return new Probe(linker.downcallHandle(library.find("armSurfaceFault").orElseThrow(),
                    FunctionDescriptor.ofVoid(JAVA_INT, JAVA_INT)),
                linker.downcallHandle(library.find("surfaceFaultCounter").orElseThrow(),
                    FunctionDescriptor.of(JAVA_INT, JAVA_INT)));
        }
        void inject(int site, int status) throws Throwable { arm.invokeExact(site, status); }
        int count(int kind) throws Throwable { return (int)counter.invokeExact(kind); }
    }

    enum Failure {
        BEGIN_FRAME(1), PREPARE_IMAGE(2), QUEUE_SUBMIT(3), PRESENT_LOST(4), OLD_FENCE(1), PRESENT_ERROR(4);
        final int operation;
        Failure(int operation) { this.operation = operation; }
    }

    @ParameterizedTest @EnumSource
    void nativeFailureReleasesSurfaceAndDestroysInflightRenderer(Failure fault) throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofConfined()) {
            var probe = Probe.load(arena);
            try (var env = new NativeEnvironment(payload, NativeEnvironment.Backend.VULKAN, NativeTestSupport.recordingLogger(new ArrayList<>()));
                 var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
                 var window = new NativeSwapchainTest.Window(); var device = new RayniumGpuDevice(runtime, renderer, true);
                 var surface = device.createSurface(window.handle, () -> false);
                 var texture = device.createTexture("source", TEXTURE_USAGE, GpuFormat.RGBA8_UNORM, 8, 4, 1, 1);
                 var view = device.createTextureView(texture); var first = new CommandTape(renderer); var oldFence = first.createFence()) {
                surface.configure(CONFIG);
                var image = (NativeTexture)texture;
                first.clearColor(new CommandTape.Region(image, 0, 0, 0, 0, 8, 4), 1, 0, 0, 1);
                assertEquals(1, first.submit().sequence());
                if (fault == Failure.BEGIN_FRAME) {
                    try (var second = new CommandTape(renderer)) { assertEquals(2, second.submit().sequence()); }
                }
                surface.acquireNextTexture();
                var committed = new AtomicInteger();
                var encoder = device.createCommandEncoder();
                if (fault != Failure.OLD_FENCE) {
                    var batch = TerrainTestPass.batch(device);
                    if (fault == Failure.PREPARE_IMAGE) {
                        batch.copyTexture(new CommandTape.Region(image, 0, 0, 0, 0, 4, 4),
                            new CommandTape.Region(image, 0, 0, 4, 0, 4, 4));
                    }
                    surface.blitFromTexture(encoder, view);
                    batch.onSubmitted(committed::incrementAndGet);
                }
                boolean genericError = fault == Failure.PRESENT_ERROR;
                probe.inject(fault.operation, genericError ? -1 : LOST);
                if (fault == Failure.OLD_FENCE) {
                    assertThrows(GpuDeviceLossException.class, () -> oldFence.awaitCompletion(-1));
                } else if (genericError) {
                    assertThrows(IllegalStateException.class, encoder::submit);
                } else {
                    assertThrows(GpuDeviceLossException.class, encoder::submit);
                }
                boolean presented = fault == Failure.PRESENT_LOST || genericError;
                boolean acquired = fault != Failure.BEGIN_FRAME && fault != Failure.OLD_FENCE;
                assertEquals(1, probe.count(0));
                assertEquals(acquired ? 1 : 0, probe.count(1));
                assertEquals(presented ? 1 : 0, probe.count(2));
                assertEquals(presented ? 1 : 0, committed.get());
                assertFalse(surface.isAcquired());
                assertThrows(IllegalStateException.class, surface::acquireNextTexture);
                assertThrows(IllegalStateException.class, renderer::pollCompleted);
                if (!genericError) assertEquals(1, probe.count(3));
            }
            if (fault != Failure.PRESENT_ERROR) assertEquals(1, probe.count(3));
        }
    }

    @Test
    void hostBytesAndQueriesPublishWhileAdjacentGpuWritesAreStillInFlight() throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofConfined();
             var env = new NativeEnvironment(payload, NativeEnvironment.Backend.VULKAN, NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var first = new CommandTape(renderer); var second = new CommandTape(renderer);
             var a = first.createFence(); var b = second.createFence();
             var host = renderer.createBuffer(GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, 8, null, () -> first);
             var mapped = host.map(true, false);
             var pool = new dev.raynium.client.renderpearl.RayniumQueryPool(renderer, 2)) {
            var library = SymbolLookup.libraryLookup(Path.of(System.getProperty("raynium.test.surfaceLayer")), arena);
            var gate = Linker.nativeLinker().downcallHandle(library.find("gateGpuBatch").orElseThrow(),
                FunctionDescriptor.of(JAVA_INT, JAVA_INT));
            var bytes = arena.allocate(JAVA_INT);
            bytes.set(JAVA_INT, 0, 0x44332211);
            first.writeBuffer(host, 0, bytes);
            first.writeTimestamp(pool, 0);
            assertEquals(1, first.submit().sequence());
            bytes.set(JAVA_INT, 0, 0x12345678);
            second.writeBuffer(host, 4, bytes);
            second.writeTimestamp(pool, 1);
            assertEquals(0, (int)gate.invokeExact(1));
            try {
                assertEquals(2, second.submit().sequence());
                bytes.fill((byte)0);
                assertTrue(a.awaitCompletion(-1));
                assertEquals(1, renderer.waitSubmit(0, 0).completedSequence());
                assertFalse(b.awaitCompletion(0));
                assertEquals(0x44332211, mapped.data().getInt(0));
                assertEquals(0, mapped.data().getInt(4));
                assertTrue(pool.getValue(0).isPresent());
                assertTrue(pool.getValue(1).isEmpty());
            } finally {
                assertEquals(0, (int)gate.invokeExact(0));
            }
            assertTrue(b.awaitCompletion(-1));
            assertEquals(0x12345678, mapped.data().getInt(4));
            assertTrue(pool.getValue(1).isPresent());
        }
    }

    enum Recovery {
        ACQUIRE_SUBOPTIMAL(5, SUBOPTIMAL), ACQUIRE_OUT_OF_DATE(5, OUT_OF_DATE), ACQUIRE_SURFACE_LOST(5, SURFACE_LOST),
        PRESENT_OUT_OF_DATE(4, OUT_OF_DATE), PRESENT_SURFACE_LOST(4, SURFACE_LOST);
        final int operation, status;
        Recovery(int operation, int status) { this.operation = operation; this.status = status; }
    }

    @ParameterizedTest @EnumSource
    void recoverableDriverResultPreservesFirstTerrainUploadAndReadbacks(Recovery fault) throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofConfined()) {
            var probe = Probe.load(arena);
            try (var env = new NativeEnvironment(payload, NativeEnvironment.Backend.VULKAN, NativeTestSupport.recordingLogger(new ArrayList<>()));
                 var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
                 var window = new NativeSwapchainTest.Window(); var device = new RayniumGpuDevice(runtime, renderer, true);
                 var surface = device.createSurface(window.handle, () -> false);
                 var texture = device.createTexture("terrain", TEXTURE_USAGE, GpuFormat.RGBA8_UNORM, 8, 4, 1, 1);
                 var view = device.createTextureView(texture);
                 var depth = renderer.createTexture(8, 4, 1, 1, NativeTexture.Format.DEPTH32F, NativeTexture.ATTACHMENT);
                 var atlas = renderer.createTexture(1, 1, 1, 1, NativeTexture.Format.RGBA8);
                 var light = renderer.createTexture(1, 1, 1, 1, NativeTexture.Format.RGBA8);
                 var queries = device.createTimestampQueryPool(1);
                 var read = device.createBuffer(null, GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, 8 * 4 * 4)) {
                surface.configure(CONFIG);
                byte[] white = {-1, -1, -1, -1};
                atlas.upload(0, 0, 0, 1, 1, white);
                light.upload(0, 0, 0, 1, 1, white);
                var encoder = device.createCommandEncoder();
                for (int pass = 0; pass < 2; ++pass) {
                    surface.acquireNextTexture();
                    var batch = TerrainTestPass.batch(device);
                    var image = (NativeTexture)texture;
                    batch.clearColor(new CommandTape.Region(image, 0, 0, 0, 0, 8, 4), 1, 0, 0, 1);
                    var frame = NativeTestSupport.solidFrame(pass + 1);
                    frame.terrainAlbedoTexture = atlas.id();
                    frame.terrainLightTexture = light.id();
                    frame.instances = List.of(new NativeRenderer.Instance(0, 0, 0, 1));
                    frame.visible = List.of(new NativeRenderer.Visible(9, -1, 0, 0, 1));
                    if (pass == 0) frame.meshes = List.of(new NativeRenderer.Mesh(9, NativeRenderPearlTest.terrainQuad(128), 8, 0));
                    var storage = batch.acquireViewStorage();
                    var snapshot = batch.addView(storage, storage.marshal(frame), atlas, light);
                    TerrainTestPass.record(batch, snapshot, image, depth, atlas, light);
                    encoder.writeTimestamp(queries, 0);
                    encoder.copyTextureToBuffer(texture, read, 0, () -> {}, 0);
                    surface.blitFromTexture(encoder, view);
                    try (var fence = encoder.createFence()) {
                        probe.inject(pass == 0 ? fault.operation : 0, fault.status);
                        encoder.submit();
                        assertEquals(pass == 0 ? 1 : 0, probe.count(0));
                        assertEquals(1, probe.count(1));
                        assertEquals(pass == 0 && fault.operation == 5 && fault.status < 0 ? 0 : 1, probe.count(2));
                        assertTrue(fence.awaitCompletion(-1));
                        assertTrue(queries.getValue(0).isPresent());
                        try (var mapped = read.slice().map(true, false)) {
                            for (int i = 0; i < 32; ++i) assertEquals(0xff000080, mapped.data().getInt(4 * i));
                        }
                    }
                    surface.present();
                    assertFalse(surface.isAcquired());
                    assertEquals(pass == 0, surface.isSuboptimal());
                    if (pass == 0 && fault.status != SUBOPTIMAL) {
                        assertThrows(SurfaceException.class, surface::acquireNextTexture);
                        surface.configure(CONFIG);
                    }
                    // Suboptimal is a hint until reconfigured. Exercise one fresh acquisition first.
                    if (pass == 0 && fault.status == SUBOPTIMAL) {
                        surface.acquireNextTexture();
                        surface.blitFromTexture(encoder, view);
                        encoder.submit();
                        surface.present();
                        surface.configure(CONFIG);
                    }
                }
            }
        }
    }
}
