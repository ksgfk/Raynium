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
import org.junit.jupiter.params.provider.ValueSource;
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

    private static final class WorkerGate implements AutoCloseable {
        private final MethodHandle control;
        WorkerGate(Arena arena,int site) throws Throwable {
            var library=SymbolLookup.libraryLookup(Path.of(System.getProperty("raynium.test.surfaceLayer")),arena);
            control=Linker.nativeLinker().downcallHandle(library.find("pauseWorker").orElseThrow(),FunctionDescriptor.of(JAVA_INT,JAVA_INT));
            assertEquals(1,(int)control.invokeExact(site));
        }
        void await() throws Throwable { assertEquals(1,(int)control.invokeExact(-1)); }
        @Override public void close() {
            try { assertEquals(1,(int)control.invokeExact(0),"Worker gate timed out"); }
            catch(RuntimeException | Error failure) { throw failure; }
            catch(Throwable failure) { throw new AssertionError(failure); }
        }
        java.util.concurrent.CompletableFuture<Void> releaseDuring(String operation) {
            var owner=Thread.currentThread();
            return java.util.concurrent.CompletableFuture.runAsync(()->{
                long deadline=System.nanoTime()+5_000_000_000L;
                while(System.nanoTime()<deadline) {
                    for(var frame:owner.getStackTrace()) if(frame.getMethodName().equals(operation)) {
                        close(); return;
                    }
                    java.util.concurrent.locks.LockSupport.parkNanos(100_000);
                }
                close();
                throw new AssertionError("Owner did not enter "+operation);
            });
        }
    }

    @Test void dequeuedPacketKeepsInputsAndFenceUntilWorkerSubmits() throws Throwable {
        var payload=NativeTestSupport.locate(directory);
        try(var arena=Arena.ofShared();
            var env=new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=new RayniumGpuDevice(runtime,renderer,true);
            var window=new NativeSwapchainTest.Window(); var chain=renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(8,4,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var pool=new dev.raynium.client.renderpearl.RayniumQueryPool(renderer,1)) {
            int source=GpuBuffer.USAGE_COPY_SRC|GpuBuffer.USAGE_MAP_WRITE;
            renderer.createBuffer(source,192,null,null).close();
            renderer.flush();
            var accessField=dev.raynium.client.terrain.RetainedTerrain.class.getDeclaredField("ACCESS");
            accessField.setAccessible(true);
            var access=(dev.raynium.client.terrain.RetainedTerrain.ViewAccess)accessField.get(null);
            var batch=device.currentBatch();
            var storage=batch.acquireViewStorage(access);
            var mesh = new dev.raynium.client.terrain.MeshPayload(1,java.nio.ByteBuffer.allocate(112),2);
            var order = java.nio.ByteBuffer.allocate(24).order(java.nio.ByteOrder.nativeOrder());
            for (int index : new int[]{0,1,2,2,3,0}) order.putInt(index);
            var indices = mesh.copyIndices(order.flip(),4);
            var verticesView = mesh.directVertices();
            var indicesView = indices.directBuffer();
            verticesView.position(verticesView.limit());
            indicesView.position(indicesView.limit());
            storage.begin(0,0,1,1,0);
            storage.terrain(texture.id(),texture.id(),1);
            storage.meshPooled(1,mesh);
            storage.indexPooled(1,indices);
            batch.addPooledView(storage,NativeTestSupport.solidFrame(1),texture,texture);
            var region=new CommandTape.Region(texture,0,0,0,0,8,4);
            batch.clearColor(region,1,0,0,1);
            batch.captureColor(region,73);
            batch.writeTimestamp(pool,0);
            batch.blitToSurface(texture,0,chain);
            var encoder=device.createCommandEncoder();
            try(var fence=batch.createFence(); var gate=new WorkerGate(arena,5)) {
                encoder.submit();
                gate.await();
                assertThrows(java.nio.ReadOnlyBufferException.class,()->verticesView.put(0,(byte)1));
                assertThrows(java.nio.ReadOnlyBufferException.class,()->indicesView.putInt(0,3));
                mesh.releaseVertices();
                indices.releaseBuffer();
                assertTrue(mesh.hasVertices());
                assertNotNull(indices.directBuffer());
                // Both pools must return a live allocation while the worker is still inside AcquireNext.
                try(var shadow=renderer.createBuffer(source,192,null,null);
                    var rotated=renderer.createFenceRotatedBuffer(source,512*1024,null,NativeBuffer.WriteSource.TRANSIENT)) {
                    assertTrue(shadow.id()>0);
                    assertTrue(rotated.id()>0);
                }
                assertTrue(renderer.pollCapture(73).isEmpty());
                assertTrue(pool.getValue(0).isEmpty());
                assertFalse(fence.awaitCompletion(0));
                var next=device.currentBatch();
                assertNotSame(storage,next.acquireViewStorage(access));
                encoder.submit();
                var release=gate.releaseDuring("waitSubmit");
                assertTrue(fence.awaitCompletion(-1));
                release.join();
                assertTrue(pool.getValue(0).isPresent());
                assertEquals(255,Byte.toUnsignedInt(renderer.pollCapture(73).orElseThrow().rgba()[0]));
                assertSame(storage,device.currentBatch().acquireViewStorage(access));
                assertFalse(mesh.hasVertices());
                assertNull(indices.directBuffer());
            }
        }
    }

    @ParameterizedTest @ValueSource(longs={0,50_000_000L})
    void coldResourcesAndEarlierFenceWorkWhileNextAcquireIsPaused(long timeout) throws Throwable {
        try(var arena=Arena.ofShared();
            var env=new NativeEnvironment(NativeTestSupport.locate(directory),NativeEnvironment.Backend.VULKAN,
                NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=new RayniumGpuDevice(runtime,renderer,true);
            var window=new NativeSwapchainTest.Window(); var chain=renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
            var output=renderer.createTexture(4,2,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var first=new CommandTape(renderer); var earlier=first.createFence(); var presenting=new CommandTape(renderer);
            var sources=new NativeRenderPearlTest.Sources()) {
            var library=SymbolLookup.libraryLookup(Path.of(System.getProperty("raynium.test.surfaceLayer")),arena);
            var atomSize=Linker.nativeLinker().downcallHandle(library.find("surfaceTestAtomSize").orElseThrow(),FunctionDescriptor.of(ValueLayout.JAVA_LONG));
            assertEquals((long)atomSize.invokeExact(),runtime.deviceInfo().transfer().hostCoherentAtomSize());
            var gpuGate=Linker.nativeLinker().downcallHandle(library.find("gateGpuBatch").orElseThrow(),FunctionDescriptor.of(JAVA_INT,JAVA_INT));
            var pipeline=com.mojang.renderpearl.api.pipeline.RenderPipeline.builder(net.minecraft.client.renderer.RenderPipelines.GUI_SNIPPET)
                .withLocation(net.minecraft.resources.Identifier.parse("raynium:test_gui")).withCull(false)
                .withPrimitiveTopology(com.mojang.renderpearl.api.pipeline.PrimitiveTopology.TRIANGLES).build();
            var pending=device.compilePipeline(pipeline,sources,Runnable::run).join();
            var encoder=device.createCommandEncoder();
            assertEquals(0,(int)gpuGate.invokeExact(1));
            boolean held=true;
            try(var gate=new WorkerGate(arena,5)) {
                var probe=Probe.load(arena);
                var submitted=new AtomicInteger();
                first.onSubmitted(submitted::incrementAndGet);
                first.submit();
                long deadline=System.nanoTime()+5_000_000_000L;
                while(submitted.get()==0 && System.nanoTime()<deadline) renderer.pollCompleted();
                assertEquals(1,submitted.get());
                // No frontend polling and no readbacks while this submission is held on the GPU.
                int samples=probe.count(5);
                Thread.sleep(40);
                assertEquals(samples,probe.count(5));
                presenting.clearColor(new CommandTape.Region(output,0,0,0,0,4,2),1,0,0,1);
                presenting.blitToSurface(output,0,chain);
                presenting.submit();
                gate.await();
                assertFalse(earlier.awaitCompletion(0));
                assertEquals(0,(int)gpuGate.invokeExact(0));
                held=false;
                assertTrue(earlier.awaitCompletion(timeout));
                try(var shader=pending.finishCompile();
                    var texture=device.createTexture("cold target",TEXTURE_USAGE,GpuFormat.RGBA8_UNORM,4,2,1,1);
                    var view=device.createTextureView(texture);
                    var cb=device.createBuffer(null,GpuBuffer.USAGE_UNIFORM,NativeRenderPearlTest.constants(true));
                    var vertices=device.createFenceRotatedBuffer(GpuBuffer.USAGE_VERTEX|GpuBuffer.USAGE_MAP_WRITE,
                        (16L<<20)+48,NativeBuffer.WriteSource.TRANSIENT);
                    var queries=device.createTimestampQueryPool(1)) {
                    byte[] red=new byte[32];
                    for(int i=0;i<32;i+=4) red[i]=red[i+3]=(byte)255;
                    ((NativeTexture)texture).upload(0,0,0,4,2,red);
                    try(var mapped=vertices.map(0,48,false,true)) {
                        var bytes=mapped.data();
                        for(float[] xy:new float[][]{{-1,-1},{3,-1},{-1,3}})
                            bytes.putFloat(xy[0]).putFloat(xy[1]).putFloat(0).putInt(-1);
                    }
                    try(var pass=encoder.createRenderPass(()->"cold resources",view,java.util.Optional.empty())) {
                        pass.setPipeline(shader); pass.setVertexBuffer(0,vertices.slice(0,48));
                        pass.setUniform("DynamicTransforms",cb); pass.setUniform("Projection",cb);
                        pass.enableScissor(0,0,2,2); pass.draw(3,1,0,0); pass.writeTimestamp(queries,0);
                    }
                    device.currentBatch().captureColor(new CommandTape.Region((NativeTexture)texture,0,0,0,0,4,2),99);
                    encoder.submit();
                    vertices.close();
                    gate.close();
                    renderer.flush();
                    assertTrue(queries.getValue(0).isPresent());
                    var bytes=renderer.pollCapture(99).orElseThrow().rgba();
                    assertEquals(32,bytes.length);
                    var pixels=java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.nativeOrder());
                    for(int i=0;i<8;++i) assertEquals(i%4<2?0xff00ff00:0xff0000ff,pixels.getInt());
                }
            } finally {
                if(held) assertEquals(0,(int)gpuGate.invokeExact(0));
            }
        }
    }

    @Test void publicViewCopiesForeignParametersWhenTheEncoderOwnsTheStorage() throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofConfined();
             var env = new NativeEnvironment(payload, NativeEnvironment.Backend.VULKAN, NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var device = new RayniumGpuDevice(runtime, renderer, true);
             var window = new NativeSwapchainTest.Window();
             var chain = renderer.createSwapchain(window.hwnd, 64, 32, NativeSwapchain.PresentMode.IMMEDIATE);
             var color = renderer.createTexture(8, 4, 1, 1, NativeTexture.Format.RGBA8, NativeTexture.ATTACHMENT | NativeTexture.READBACK);
             var depth = renderer.createTexture(8, 4, 1, 1, NativeTexture.Format.DEPTH32F, NativeTexture.ATTACHMENT);
             var atlas = renderer.createTexture(1, 1, 1, 1, NativeTexture.Format.RGBA8);
             var light = renderer.createTexture(1, 1, 1, 1, NativeTexture.Format.RGBA8);
             var first = new CommandTape(renderer)) {
            byte[] white = {-1, -1, -1, -1};
            atlas.upload(0, 0, 0, 1, 1, white);
            light.upload(0, 0, 0, 1, 1, white);
            var region = new CommandTape.Region(color, 0, 0, 0, 0, 8, 4);
            first.clearColor(region, 0, 0, 0, 1);
            first.blitToSurface(color, 0, chain);
            var encoder = device.createCommandEncoder();
            try (var gate = new WorkerGate(arena, 5)) {
                first.submit();
                gate.await();
                var second = device.currentBatch();
                var pooled = second.acquireViewStorage();
                try (var external = new NativeFrameBuffer()) {
                    var frame = NativeTestSupport.solidFrame(2);
                    frame.terrainAlbedoTexture = atlas.id();
                    frame.terrainLightTexture = light.id();
                    frame.instances = List.of(new NativeRenderer.Instance(0, 0, 0, 1));
                    frame.visible = List.of(new NativeRenderer.Visible(9, -1, 0, 0, 1));
                    frame.meshes = List.of(new NativeRenderer.Mesh(9, NativeRenderPearlTest.terrainQuad(128), 8, 0));
                    var view = second.addView(pooled, external.marshal(frame), atlas, light);
                    TerrainTestPass.record(second, view, color, depth, atlas, light);
                    second.captureColor(region, 61);
                    encoder.submit();
                    // The worker is still processing the previous packet. Make a stale borrow observable.
                    external.visibleSegment().fill((byte) 0);
                }
            }
            renderer.flush();
            var rgba = renderer.pollCapture(61).orElseThrow().rgba();
            var pixels = java.nio.ByteBuffer.wrap(rgba).order(java.nio.ByteOrder.nativeOrder()).asIntBuffer();
            assertEquals(32, pixels.remaining());
            while (pixels.hasRemaining()) assertEquals(0xff000080, pixels.get());
        }
    }

    @Test void preflightFailurePreservesPreviousBatchReadbacksInsideAndAfterTerminalCallback() throws Throwable {
        var payload=NativeTestSupport.locate(directory);
        try(var arena=Arena.ofShared();
            var env=new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var window=new NativeSwapchainTest.Window(); var chain=renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(8,4,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var pool=new dev.raynium.client.renderpearl.RayniumQueryPool(renderer,1);
            var read=renderer.createBuffer(GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,4,null,null);
            var tape=new CommandTape(renderer); var gate=new WorkerGate(arena,4)) {
            var region=new CommandTape.Region(texture,0,0,0,0,8,4);
            var bytes=arena.allocate(JAVA_INT);
            bytes.set(JAVA_INT,0,17);
            tape.writeBuffer(read,0,bytes.asSlice(0,4));
            tape.clearColor(region,1,0,0,1); tape.captureColor(region,19);
            tape.writeTimestamp(pool,0); tape.blitToSurface(texture,0,chain);
            var completed=new AtomicInteger();
            tape.onCompleted(()->{assertTrue(pool.getValue(0).isPresent()); completed.incrementAndGet();});
            tape.submit();
            gate.await();
            assertEquals(0,renderer.waitSubmit(1,0).completedSequence());
            assertTrue(pool.getValue(0).isEmpty());
            assertTrue(renderer.pollCapture(19).isEmpty());
            assertEquals(0,completed.get());
            var bad=arena.allocate(8,8);
            bad.set(JAVA_INT,0,-1); bad.set(JAVA_INT,4,8);
            var release=gate.releaseDuring("submitFrame");
            assertThrows(IllegalStateException.class,()->renderer.submitFrame(bad,MemorySegment.NULL,null));
            release.join();
            assertEquals(1,completed.get());
            assertTrue(pool.getValue(0).isPresent());
            assertEquals(255,Byte.toUnsignedInt(renderer.pollCapture(19).orElseThrow().rgba()[0]));
            try(var map=read.slice().map(true,false)) { assertEquals(17,map.data().getInt(0)); }
            assertThrows(IllegalStateException.class,renderer::pollCompleted);
            assertThrows(IllegalStateException.class,()->renderer.createQueryPool(1));
        }
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
                if(fault==Failure.OLD_FENCE) probe.inject(6,1);
                assertEquals(1, first.submit().sequence());
                if (fault == Failure.BEGIN_FRAME) {
                    try (var second = new CommandTape(renderer)) { assertEquals(2, second.submit().sequence()); }
                }
                if(fault!=Failure.OLD_FENCE) renderer.flush();
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
                    assertThrows(IllegalStateException.class, ()->{ encoder.submit(); renderer.flush(); });
                } else {
                    assertThrows(GpuDeviceLossException.class, ()->{ encoder.submit(); renderer.flush(); });
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
                if(fault==Failure.OLD_FENCE) probe.inject(6,0);
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
            first.writeTimestamp(pool, 1);
            assertEquals(1, first.submit().sequence());
            assertTrue(a.awaitCompletion(-1));
            assertTrue(pool.getValue(1).isPresent());
            bytes.set(JAVA_INT, 0, 0x12345678);
            second.writeBuffer(host, 4, bytes);
            second.writeTimestamp(pool, 1);
            var callbacks = new AtomicInteger();
            second.onCompleted(() -> {
                assertEquals(0x12345678, mapped.data().getInt(4));
                callbacks.incrementAndGet();
            });
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
                var sample = arena.allocate(16, 8);
                var progress = arena.allocate(NativeBindings.WAIT_BYTES, 8);
                assertEquals(0, (int)renderer.bindings.syncQueryPool.invokeExact(NativeTestSupport.handle(renderer),
                        TerrainTestPass.queryPoolId(pool), 1, 1, sample, progress));
                assertEquals(0L, sample.get(ValueLayout.JAVA_LONG, 0));
                assertTrue(pool.getValue(1).isEmpty());
            } finally {
                assertEquals(0, (int)gate.invokeExact(0));
            }
            assertTrue(b.awaitCompletion(-1));
            assertEquals(1, callbacks.get());
            assertEquals(0x12345678, mapped.data().getInt(4));
            assertTrue(pool.getValue(1).isPresent());
        }
    }

    @Test void closingRendererPublishesCompletedReadbackAfterInputWasConsumed() throws Throwable {
        try (var arena = Arena.ofConfined();
             var env = new NativeEnvironment(NativeTestSupport.locate(directory), NativeEnvironment.Backend.VULKAN,
                 NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var host = renderer.createBuffer(GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, 4, null, null);
             var tape = new CommandTape(renderer)) {
            var library = SymbolLookup.libraryLookup(Path.of(System.getProperty("raynium.test.surfaceLayer")), arena);
            var gate = Linker.nativeLinker().downcallHandle(library.find("gateGpuBatch").orElseThrow(),
                FunctionDescriptor.of(JAVA_INT, JAVA_INT));
            var submitted = new AtomicInteger();
            var completed = new AtomicInteger();
            var bytes = arena.allocate(JAVA_INT);
            bytes.set(JAVA_INT, 0, 0x12345678);
            tape.writeBuffer(host, 0, bytes);
            tape.onSubmitted(submitted::incrementAndGet);
            tape.onCompleted(() -> {
                try (var mapping = host.map(true, false)) {
                    assertEquals(0x12345678, mapping.data().getInt(0));
                }
                completed.incrementAndGet();
            });
            assertEquals(0, (int)gate.invokeExact(1));
            try {
                tape.submit();
                long deadline = System.nanoTime() + 5_000_000_000L;
                while (submitted.get() == 0 && System.nanoTime() < deadline) {
                    renderer.pollCompleted();
                    Thread.onSpinWait();
                }
                assertEquals(1, submitted.get());
                assertEquals(0, completed.get());
            } finally {
                assertEquals(0, (int)gate.invokeExact(0));
            }
            // No intervening frontend poll. Closing must observe the final GPU completion itself.
            renderer.close();
            assertEquals(1, completed.get());
        }
    }

    @Test void missingTimingInFirstRetiredSlotPreservesSubmissionOrder() throws Throwable {
        try (var arena=Arena.ofConfined();
             var env=new NativeEnvironment(NativeTestSupport.locate(directory),NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders())) {
            var probe=Probe.load(arena);
            probe.inject(9,1);
            try {
                var empty=MemorySegment.NULL;
                renderer.submitFrame(empty,empty,null);
                long first=renderer.submitFrame(empty,empty,empty,null,List.of(),true).sequence();
                assertTrue(renderer.awaitSubmission(first,-1));
                probe.inject(8,-5);
                long second=renderer.submitFrame(empty,empty,empty,null,List.of(),true).sequence();
                assertTrue(renderer.awaitSubmission(second,-1));
                // WaitIdle visits slot zero (the newer batch) before slot one.
                renderer.flush();
                var records=arena.allocate(304,8);
                var count=arena.allocate(JAVA_INT);
                var handle=NativeTestSupport.handle(renderer);
                assertEquals(0,(int)renderer.bindings.drainTelemetry.invokeExact(handle,records,2,count));
                assertEquals(2,count.get(JAVA_INT,0));
                assertTrue(probe.count(0)>0);
                assertEquals(first,records.get(ValueLayout.JAVA_LONG,0));
                assertEquals(1,records.get(JAVA_INT,8));
                assertEquals(second,records.get(ValueLayout.JAVA_LONG,152));
                assertEquals(0,records.get(JAVA_INT,160));
                assertEquals(0,records.get(ValueLayout.JAVA_LONG,168));
            } finally { probe.inject(9,0); probe.inject(0,0); }
        }
    }

    enum TailFault { READBACK_MAP, DEVICE_LOST_COUNTER, DEVICE_LOST_DURING_FLUSH }

    @ParameterizedTest @EnumSource
    void tailReadbackFailureKeepsLastCompleteBatch(TailFault fault) throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofShared();
             var env = new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime()) {
            try (var renderer = runtime.createRenderer(NativeTestSupport.shaders());
                 var window = new NativeSwapchainTest.Window(); var chain = renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
                 var texture = renderer.createTexture(8,4,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
                 var old = new CommandTape(renderer); var pending = new CommandTape(renderer)) {
                var probe = Probe.load(arena);
                var region = new CommandTape.Region(texture,0,0,0,0,8,4);
                probe.inject(9,1);
                try {
                    old.clearColor(region,1,0,0,1);
                    old.captureColor(region,81);
                    long previous = TerrainTestPass.submitMeasured(old);
                    renderer.flush();
                    var callbacks = new AtomicInteger();
                    pending.clearColor(region,0,1,0,1);
                    pending.captureColor(region,82);
                    pending.onCompleted(callbacks::incrementAndGet);
                    long seq;
                    boolean flushing = fault == TailFault.DEVICE_LOST_DURING_FLUSH;
                    if (fault == TailFault.READBACK_MAP) {
                        // The first reservation reuses the previous batch's staging buffer. The second needs a new mapping.
                        pending.captureColor(region,83);
                        probe.inject(8,-5);
                        seq = TerrainTestPass.submitMeasured(pending);
                        assertThrows(IllegalStateException.class,()->renderer.awaitSubmission(seq,-1));
                    } else {
                        pending.blitToSurface(texture,0,chain);
                        try (var gate = new WorkerGate(arena,4)) {
                            seq = pending.submit().sequence();
                            gate.await();
                            probe.inject(1,LOST);
                            var release = gate.releaseDuring(flushing ? "flush" : "waitSubmit");
                            if (flushing) assertThrows(GpuDeviceLossException.class, renderer::flush);
                            else assertThrows(GpuDeviceLossException.class,()->renderer.awaitSubmission(seq,-1));
                            release.join();
                        }
                    }
                    if (fault==TailFault.READBACK_MAP) assertTrue(probe.count(0)>0);
                    else assertEquals(1,probe.count(0));
                    var progress = arena.allocate(48,8);
                    var handle = NativeTestSupport.handle(renderer);
                    if (flushing) assertEquals(1,(int)renderer.bindings.flushRenderer.invokeExact(handle,progress));
                    else assertEquals(1,(int)renderer.bindings.waitSubmit.invokeExact(handle,0L,0L,progress));
                    var snapshot = NativeRenderer.Progress.read(progress,0);
                    assertEquals(previous,snapshot.completed());
                    assertEquals(fault == TailFault.READBACK_MAP ? previous : seq,snapshot.submitted());
                    assertEquals(fault == TailFault.READBACK_MAP ? 2 : 3,snapshot.flags());
                    assertEquals(0,callbacks.get());
                    var measured = new ArrayList<Long>();
                    renderer.drainTelemetry(record->measured.add(record.get(ValueLayout.JAVA_LONG,0)));
                    assertEquals(List.of(previous),measured);
                    assertEquals(255,Byte.toUnsignedInt(renderer.pollCapture(81).orElseThrow().rgba()[0]));
                    assertThrows(IllegalStateException.class,()->renderer.pollCapture(82));
                } finally {
                    probe.inject(9,0);
                    probe.inject(0,0);
                }
            }
            if (fault==TailFault.READBACK_MAP) {
                try (var healthy=runtime.createRenderer(NativeTestSupport.shaders());
                     var target=healthy.createTexture(8,4,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
                     var retry=new CommandTape(healthy)) {
                    var region=new CommandTape.Region(target,0,0,0,0,8,4);
                    retry.clearColor(region,0,0,1,1);
                    retry.captureColor(region,82);
                    retry.submit();
                    healthy.flush();
                    assertEquals(255,Byte.toUnsignedInt(healthy.pollCapture(82).orElseThrow().rgba()[2]));
                }
            }
        }
    }

    @Test void deviceLossDuringFirstSwapchainCreationClosesFastPaths() throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofShared();
             var env = new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var window = new NativeSwapchainTest.Window()) {
            var probe = Probe.load(arena);
            probe.inject(7,LOST);
            try {
                assertThrows(IllegalStateException.class,()->renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE));
                assertTrue(probe.count(4)>0);
                assertThrows(IllegalStateException.class,()->renderer.createQueryPool(1));
                assertThrows(GpuDeviceLossException.class,renderer::pollCompleted);
            } finally { probe.inject(7,0); }
        }
    }

    @Test void deviceLossDuringReplacementSwapchainCreationPoisonsRenderer() throws Throwable {
        var payload = NativeTestSupport.locate(directory);
        try (var arena = Arena.ofShared();
             var env = new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var window = new NativeSwapchainTest.Window(); var chain = renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
             var texture = renderer.createTexture(8,4,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
             var tape = new CommandTape(renderer)) {
            var probe = Probe.load(arena);
            tape.clearColor(new CommandTape.Region(texture,0,0,0,0,8,4),1,0,0,1);
            tape.blitToSurface(texture,0,chain);
            probe.inject(4,SURFACE_LOST);
            tape.submit();
            assertEquals(5,renderer.flush());
            probe.inject(7,LOST);
            try {
                assertThrows(IllegalStateException.class,()->chain.resize(96,48,NativeSwapchain.PresentMode.IMMEDIATE));
                assertTrue(probe.count(4)>0);
                assertThrows(GpuDeviceLossException.class,renderer::pollCompleted);
            } finally { probe.inject(7,0); }
        }
    }

    enum ResizeFailure { RECREATE_RETRY, RECREATE_CLOSE, LOST_RETRY, LOST_CLOSE }

    @ParameterizedTest @EnumSource
    void resizeFailureKeepsOuterHandleAndOffscreenWork(ResizeFailure scenario) throws Throwable {
        var payload=NativeTestSupport.locate(directory);
        try(var arena=Arena.ofShared();
            var env=new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var window=new NativeSwapchainTest.Window(); var chain=renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
            var image=renderer.createTexture(8,4,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK)) {
            var probe=Probe.load(arena);
            var region=new CommandTape.Region(image,0,0,0,0,8,4);
            probe.inject(7,-1);
            try {
                if(scenario==ResizeFailure.LOST_RETRY || scenario==ResizeFailure.LOST_CLOSE) {
                    try(var gate=new WorkerGate(arena,4); var tape=new CommandTape(renderer)) {
                        tape.clearColor(region,1,0,0,1); tape.blitToSurface(image,0,chain);
                        tape.submit(); gate.await();
                        probe.inject(4,SURFACE_LOST);
                        var release=gate.releaseDuring("resize");
                        assertThrows(IllegalStateException.class,()->chain.resize(96,48,NativeSwapchain.PresentMode.IMMEDIATE));
                        release.join();
                    }
                } else {
                    assertThrows(IllegalStateException.class,()->chain.resize(96,48,NativeSwapchain.PresentMode.IMMEDIATE));
                }
            } finally {
                probe.inject(7,0);
            }
            assertTrue(probe.count(4)>0,"Expected the driver layer to observe a swapchain creation");
            try(var tape=new CommandTape(renderer)) {
                tape.clearColor(region,0,1,0,1); tape.captureColor(region,53);
                tape.blitToSurface(image,0,chain);
                tape.submit();
                assertEquals(5,renderer.flush());
                assertEquals(255,Byte.toUnsignedInt(renderer.pollCapture(53).orElseThrow().rgba()[1]));
            }
            if(scenario==ResizeFailure.LOST_RETRY || scenario==ResizeFailure.RECREATE_RETRY) {
                chain.resize(96,48,NativeSwapchain.PresentMode.IMMEDIATE);
                assertEquals(0,NativeTestSupport.blit(renderer,chain));
            }
        }
    }

    @Test void skippedPresentPreservesAnUnobservedLossAndSuccessfulPresentsAccumulate() throws Throwable {
        try (var arena = Arena.ofConfined();
             var env = new NativeEnvironment(NativeTestSupport.locate(directory), NativeEnvironment.Backend.VULKAN,
                     NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var window = new NativeSwapchainTest.Window(); var device = new RayniumGpuDevice(runtime, renderer, true);
             var surface = device.createSurface(window.handle, () -> false);
             var color = renderer.createTexture(8, 4, 1, 1, NativeTexture.Format.RGBA8, NativeTexture.READBACK | NativeTexture.ATTACHMENT);
             var tape = new CommandTape(renderer)) {
            surface.configure(CONFIG);
            tape.clearColor(new CommandTape.Region(color, 0, 0, 0, 0, 8, 4), 1, 0, 0, 1);
            tape.submit();
            renderer.flush();
            var probe = Probe.load(arena);
            var handle = NativeTestSupport.handle(renderer);
            var result = arena.allocate(NativeBindings.WAIT_BYTES, 8);
            var blit = arena.allocateFrom(JAVA_INT, 6, 16, color.id(), 0);
            var empty = MemorySegment.NULL;
            // Observe both C results without publishing either snapshot to the Java surface.
            probe.inject(4, SURFACE_LOST);
            assertTrue(renderer.submitFrame(blit, empty, empty, renderer.swapchain, List.of(), false, null).success());
            assertEquals(0, (int)renderer.bindings.flushRenderer.invokeExact(handle, result));
            assertEquals(5, NativeRenderer.Progress.read(result, 0).outcome());
            assertTrue(org.lwjgl.sdl.SDLVideo.SDL_HideWindow(window.handle));
            window.pump();
            assertTrue(renderer.submitFrame(blit, empty, empty, renderer.swapchain, List.of(), false, null).success());
            assertEquals(0, (int)renderer.bindings.flushRenderer.invokeExact(handle, result));
            var skipped = NativeRenderer.Progress.read(result, 0);
            assertEquals(1, skipped.outcome());
            assertEquals(5, skipped.recovery());
            assertThrows(SurfaceException.class, surface::acquireNextTexture);
            assertTrue(org.lwjgl.sdl.SDLVideo.SDL_ShowWindow(window.handle));
            window.pump();
            surface.configure(CONFIG);
            for (int i = 0; i < 2; ++i)
                assertTrue(renderer.submitFrame(blit, empty, empty, renderer.swapchain, List.of(), false, null).success());
            assertEquals(0, (int)renderer.bindings.flushRenderer.invokeExact(handle, result));
            var presented = NativeRenderer.Progress.read(result, 0);
            assertEquals(skipped.presented() + 2, presented.presented());
            renderer.syncWorkerState(presented);
            for (int i = 0; i < 2; ++i) {
                assertEquals(0, (int)renderer.bindings.waitSubmit.invokeExact(handle, 0L, 0L, result));
                assertEquals(presented.presented(), NativeRenderer.Progress.read(result, 0).presented());
                renderer.pollCompleted();
            }
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
                        renderer.flush();
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
