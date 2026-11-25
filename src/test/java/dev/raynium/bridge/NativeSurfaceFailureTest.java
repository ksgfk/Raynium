package dev.raynium.bridge;

import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.device.*;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.renderpearl.TerrainTestPass;
import java.lang.foreign.*;
import java.lang.invoke.*;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.lwjgl.sdl.SDLVideo;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeSurfaceFailureTest {
    @TempDir Path directory;
    private static final GpuSurface.Configuration CONFIG=new GpuSurface.Configuration(64,32,GpuSurface.PresentMode.IMMEDIATE);

    /** Change only the returned status at the C boundary. The device, work, waits and destruction use the staged DLL. */
    private static final class Driver {
        final NativeBindings bindings;
        private final MethodHandle submit,wait;
        int outcome=-1, status, flags, submits, waits;
        boolean failBeforeSubmit,loseOnWait;
        long lastSequence;
        boolean targetedSurface;
        Driver(Arena arena,Path dll) throws ReflectiveOperationException {
            var symbols=SymbolLookup.libraryLookup(NativeLibrary.windowsLibraryPath(dll),arena);
            var linker=Linker.nativeLinker();
            var sub=FunctionDescriptor.of(JAVA_INT,ADDRESS,ADDRESS,ADDRESS);
            var await=FunctionDescriptor.of(JAVA_INT,ADDRESS,JAVA_LONG,JAVA_LONG,ADDRESS);
            submit=linker.downcallHandle(symbols.find("rn_submit_frame").orElseThrow(),sub);
            wait=linker.downcallHandle(symbols.find("rn_wait_submit").orElseThrow(),await);
            var lookup=MethodHandles.lookup();
            var subStub=linker.upcallStub(lookup.findVirtual(Driver.class,"submit",sub.toMethodType()).bindTo(this),sub,arena);
            var waitStub=linker.upcallStub(lookup.findVirtual(Driver.class,"waitSubmit",await.toMethodType()).bindTo(this),await,arena);
            bindings=new NativeBindings(name->switch(name) {
                case "rn_submit_frame"->Optional.of(subStub);
                case "rn_wait_submit"->Optional.of(waitStub);
                default->symbols.find(name);
            });
        }
        private int submit(MemorySegment renderer,MemorySegment batch,MemorySegment output) throws Throwable {
            ++submits;
            targetedSurface=batch.reinterpret(96).get(ADDRESS,8).address()!=0;
            var result=output.reinterpret(NativeBindings.SUBMIT_BYTES);
            if(failBeforeSubmit) result.fill((byte)0);
            else if((int)submit.invokeExact(renderer,batch,output)!=0) return 1;
            lastSequence=result.get(JAVA_LONG,0);
            if(status!=0 && lastSequence!=0) {
                var progress=result.asSlice(8);
                if((int)wait.invokeExact(renderer,lastSequence,-1L,progress)!=0) return 1;
            }
            apply(result.asSlice(8));
            if(status!=0) result.set(JAVA_INT,52,flags|2);
            return status;
        }
        private void apply(MemorySegment progress) {
            if(outcome<0 || !targetedSurface) return;
            progress.set(JAVA_INT,36,outcome==0?0:outcome);
            progress.set(JAVA_INT,40,outcome);
        }
        private int waitSubmit(MemorySegment renderer,long sequence,long timeout,MemorySegment output) throws Throwable {
            ++waits;
            int status=(int)wait.invokeExact(renderer,sequence,timeout,output);
            var progress=output.reinterpret(NativeBindings.WAIT_BYTES);
            apply(progress);
            if(loseOnWait) { progress.set(JAVA_INT,44,3); return 1; }
            return status;
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void deferredSurfaceResultsPreserveOffscreenWorkAndAllowReconfiguration(NativeEnvironment.Backend api) throws Exception {
        var payload=NativeTestSupport.locate(directory);
        try(var env=new NativeEnvironment(payload,api,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var scratch=Arena.ofConfined(); var window=new NativeSwapchainTest.Window()) {
            var d=new Driver(scratch,payload.libraryPath());
            try(var runtime=env.createRuntime(d.bindings); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
                var device=new RayniumGpuDevice(runtime,renderer,true);
                var surface=device.createSurface(window.handle,()->false);
                var texture=device.createTexture("display",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,8,4,1,1);
                var view=device.createTextureView(texture);
                var depth=renderer.createTexture(8,4,1,1,NativeTexture.Format.DEPTH32F,NativeTexture.ATTACHMENT);
                var atlas=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
                var light=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
                var queries=device.createTimestampQueryPool(1);
                var read=device.createBuffer(null,com.mojang.renderpearl.api.buffers.GpuBuffer.USAGE_MAP_READ|
                    com.mojang.renderpearl.api.buffers.GpuBuffer.USAGE_COPY_DST,8*4*4)) {
                byte[] white={-1,-1,-1,-1};
                atlas.upload(0,0,0,1,1,white); light.upload(0,0,0,1,1,white);
                var encoder=device.createCommandEncoder();
                surface.configure(CONFIG);
                for(int outcome:new int[]{4,2,5,0}) {
                    surface.acquireNextTexture();
                    encoder.submit();
                    assertTrue(surface.isAcquired());
                    assertFalse(d.targetedSurface);
                    d.outcome=outcome;
                    if(outcome==5) SDLVideo.SDL_HideWindow(window.handle);
                    var batch=TerrainTestPass.batch(device);
                    var image=(NativeTexture)texture;
                    var area=new dev.raynium.client.renderpearl.CommandTape.Region(image,0,0,0,0,8,4);
                    batch.clearColor(area,1,0,0,1);
                    boolean terrain=outcome==5 || outcome==0;
                    if(terrain) {
                        var frame=NativeTestSupport.solidFrame(outcome==5?1:2);
                        frame.terrainAlbedoTexture=atlas.id(); frame.terrainLightTexture=light.id();
                        frame.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                        frame.visible=List.of(new NativeRenderer.Visible(9,-1,0,0,1));
                        if(outcome==5) frame.meshes=List.of(new NativeRenderer.Mesh(9,NativeRenderPearlTest.terrainQuad(128),8,0));
                        var storage=batch.acquireViewStorage();
                        var snapshot=batch.addView(storage,storage.marshal(frame),atlas,light);
                        TerrainTestPass.record(batch,snapshot,image,depth,atlas,light);
                        encoder.writeTimestamp(queries,0);
                        encoder.copyTextureToBuffer(texture,read,0,()->{},0);
                    }
                    batch.captureColor(area,100+outcome);
                    surface.blitFromTexture(encoder,view);
                    try(var fence=encoder.createFence()) {
                        encoder.submit();
                        assertTrue(d.targetedSurface);
                        assertTrue(fence.awaitCompletion(-1));
                        assertEquals((byte)(terrain?128:255),renderer.pollCapture(100+outcome).orElseThrow().rgba()[0]);
                        if(terrain) {
                            assertTrue(queries.getValue(0).isPresent());
                            try(var mapped=read.slice().map(true,false)) {
                                for(int i=0;i<32;++i) assertEquals(0xff000080,mapped.data().getInt(4*i));
                            }
                        }
                    }
                    d.outcome=-1;
                    encoder.submit();
                    assertFalse(d.targetedSurface);
                    surface.present();
                    assertFalse(surface.isAcquired());
                    assertEquals(outcome!=0,surface.isSuboptimal());
                    if(outcome==2 || outcome==5) assertThrows(SurfaceException.class,surface::acquireNextTexture);
                    SDLVideo.SDL_ShowWindow(window.handle);
                    surface.configure(CONFIG);
                    assertFalse(surface.isSuboptimal());
                }
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void terminalSubmitAndOldFenceReleaseAcquiredSurfaceBeforeRealNativeClose(NativeEnvironment.Backend api) throws Exception {
        var payload=NativeTestSupport.locate(directory);
        try(var env=new NativeEnvironment(payload,api,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var scratch=Arena.ofConfined(); var window=new NativeSwapchainTest.Window()) {
            var d=new Driver(scratch,payload.libraryPath());
            for(int fault=0;fault<4;++fault) {
                d.status=d.flags=0; d.failBeforeSubmit=d.loseOnWait=false;
                try(var runtime=env.createRuntime(d.bindings); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
                    var device=new RayniumGpuDevice(runtime,renderer,true);
                    var surface=device.createSurface(window.handle,()->false);
                    var texture=device.createTexture("display",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,8,4,1,1);
                    var view=device.createTextureView(texture)) {
                    surface.configure(CONFIG);
                    var encoder=device.createCommandEncoder();
                    try(var oldBatch=new dev.raynium.client.renderpearl.CommandTape(renderer); var old=oldBatch.createFence()) {
                        oldBatch.submit(); // Keep the result unpublished until the explicit wait below.
                        surface.acquireNextTexture();
                        // An unconfigured frontend shares the device without owning its native swapchain.
                        try(var unused=device.createSurface(window.handle,()->false)) { assertFalse(unused.isAcquired()); }
                        if(fault==3) {
                            d.loseOnWait=true;
                            assertThrows(GpuDeviceLossException.class,renderer::pollCompleted);
                        } else {
                            surface.blitFromTexture(encoder,view);
                            d.status=1; d.flags=fault==2?1:0;
                            d.failBeforeSubmit=fault==0;
                            var committed=new AtomicInteger();
                            TerrainTestPass.batch(device).onSubmitted(committed::incrementAndGet);
                            if(fault==2) assertThrows(GpuDeviceLossException.class,encoder::submit);
                            else assertThrows(IllegalStateException.class,encoder::submit);
                            assertEquals(fault==0?0:1,committed.get());
                            assertEquals(fault!=0,d.lastSequence>0);
                        }
                        assertFalse(surface.isAcquired());
                        assertThrows(IllegalStateException.class,surface::acquireNextTexture);
                        assertFalse(surface.isAcquired());
                        int submissions=d.submits;
                        assertThrows(IllegalStateException.class,renderer::pollCompleted);
                        assertEquals(submissions,d.submits);
                    }
                }
            }
        }
    }
}
