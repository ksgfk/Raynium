package dev.raynium.bridge;

import com.mojang.blaze3d.buffers.Std140Builder;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumQueryPool;
import dev.raynium.client.renderpearl.CommandTape.Region;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;
import java.util.ArrayList;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeCommandTapeTest {
    @TempDir Path directory;

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void headlessReplayPreservesSubresourceOrderAndRecordingLifetimes(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var texture=renderer.createTexture(8,4,2,2,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var tape=new CommandTape(renderer); var fence=tape.createFence()) {
            var whole=new Region(texture,1,1,0,0,4,2);
            var left=new Region(texture,1,1,0,0,2,2);
            var right=new Region(texture,1,1,2,0,2,2);
            tape.clearColor(whole,1,0,0,1);
            tape.captureColor(right,101);
            assertThrows(IllegalArgumentException.class,()->tape.copyTexture(left,left));
            tape.clearColor(left,0,1,0,1);
            tape.copyTexture(left,right);
            byte[] blue={0,0,-1,-1,99,99,99,99,0,0,-1,-1};
            tape.uploadTexture(new Region(texture,1,1,3,0,1,2),8,MemorySegment.ofArray(blue));
            java.util.Arrays.fill(blue,(byte)0);
            tape.captureColor(whole,102);
            texture.close();
            assertFalse(fence.awaitCompletion(0));
            assertThrows(IllegalStateException.class,()->fence.awaitCompletion(-1));
            var result=tape.submit();
            assertTrue(result.success());
            assertEquals(1,result.sequence());
            assertEquals(3,result.outcome());
            assertTrue(fence.awaitCompletion(-1));
            assertTrue(fence.awaitCompletion(0));
            var before=renderer.pollCapture(101).orElseThrow();
            assertEquals(2,before.width());
            assertEquals(2,before.height());
            checkPixels(before.rgba(),0xff0000ff,0xff0000ff);
            var after=renderer.pollCapture(102).orElseThrow();
            assertEquals(4,after.width());
            assertEquals(2,after.height());
            checkPixels(after.rgba(),0xff00ff00,0xff00ff00,0xff00ff00,0xffff0000);
            try(var next=new CommandTape(renderer); var other=next.createFence()) {
                assertEquals(2,next.submit().sequence());
                assertTrue(other.awaitCompletion(Long.MAX_VALUE));
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void malformedTapePoisonsRendererWithoutPublishingASequence(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders()); var arena=Arena.ofConfined()) {
            var command=arena.allocate(8,8);
            command.set(ValueLayout.JAVA_INT,0,-1);
            command.set(ValueLayout.JAVA_INT,4,8);
            var result=renderer.submitFrame(command,MemorySegment.NULL,null);
            assertFalse(result.success());
            assertFalse(result.deviceLost());
            assertEquals(0,result.sequence());
            assertFalse(renderer.submitFrame(MemorySegment.NULL,MemorySegment.NULL,null).success());
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void surfaceBlitUsesOnlyItsOwningBatch(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var surface=renderer.createSwapchain(window.hwnd,64,32,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(32,16,2,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var pre=new CommandTape(renderer); var display=new CommandTape(renderer); var post=new CommandTape(renderer)) {
            pre.clearColor(new Region(texture,1,0,0,0,16,8),1,0,0,1);
            assertEquals(3,pre.submit().outcome());
            display.blitToSurface(texture,1,surface);
            var shown=display.submit();
            assertTrue(shown.success());
            assertEquals(2,shown.sequence());
            assertTrue(shown.outcome()==0 || shown.outcome()==4,"surface outcome "+shown.outcome());
            post.captureColor(new Region(texture,1,0,0,0,1,1),103);
            var after=post.submit();
            assertEquals(3,after.sequence());
            assertEquals(3,after.outcome());
            assertTrue(renderer.awaitSubmission(after.sequence(),-1));
            assertArrayEquals(new byte[]{-1,0,0,-1},renderer.pollCapture(103).orElseThrow().rgba());
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void publishesOnlyLatestLogicalTimestampAcrossSlotReuse(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var pool=new RayniumQueryPool(renderer,3);
            var texture=renderer.createTexture(8,8,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT)) {
            assertTrue(pool.getValue(0).isEmpty());
            for(int batch=0;batch<4;batch++) {
                try(var tape=new CommandTape(renderer); var fence=tape.createFence()) {
                    assertThrows(IllegalStateException.class,()->tape.writeTimestamp(pool,3));
                    tape.writeTimestamp(pool,0);
                    tape.clearColor(new Region(texture,0,0,0,0,8,8),1,1,1,1);
                    tape.writeTimestamp(pool,1);
                    tape.writeTimestamp(pool,0);
                    assertTrue(pool.getValue(0).isEmpty());
                    assertEquals(batch+1,tape.submit().sequence());
                    if(batch%2==1) {
                        assertTrue(fence.awaitCompletion(-1));
                        var results=pool.getValues(0,3);
                        assertTrue(results[0].orElseThrow()>=results[1].orElseThrow());
                        assertTrue(results[1].getAsLong()>0);
                        assertTrue(results[2].isEmpty());
                    }
                }
            }
            try(var cancelled=new CommandTape(renderer)) {
                cancelled.writeTimestamp(pool,1);
            }
            assertTrue(pool.getValue(1).isEmpty());
            assertTrue(pool.getValue(0).isPresent());
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void publishesHostWritesInTapeOrderAndFlushesOpenUploadViews(NativeEnvironment.Backend backend) throws Exception {
        var current=new java.util.concurrent.atomic.AtomicReference<CommandTape>();
        var initial=java.nio.ByteBuffer.allocate(20).order(java.nio.ByteOrder.nativeOrder());
        initial.asIntBuffer().put(new int[]{-1,10,20,30,40});
        initial.position(4);
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=renderer.createBuffer(2|8|16,16,initial,current::get);
            var upload=renderer.createBuffer(2|16,8,null,current::get);
            var host=renderer.createBuffer(1|8,16,null,current::get);
            var open=upload.map(false,true); var visible=host.map(true,false);
            var pre=new CommandTape(renderer); var next=new CommandTape(renderer); var done=next.createFence()) {
            assertThrows(UnsupportedOperationException.class,()->renderer.createBuffer(1|2,4,null,current::get));
            initial.putInt(4,-2);
            assertTrue(visible.data().isReadOnly());
            assertEquals(java.nio.ByteOrder.nativeOrder(),visible.data().order());
            assertEquals(0,visible.data().getInt(0));
            var old=upload.map(4,4,false,true);
            var oldBytes=old.data();
            old.close();
            try(var replacement=upload.map(4,4,false,true)) {
                old.close();
                assertThrows(IllegalStateException.class,()->oldBytes.getInt(0));
                replacement.data().putInt(0,42);
            }
            open.data().putInt(0,77);
            open.data().putInt(4,88);
            current.set(pre);
            var crossing=device.map(4,4,false,true);
            crossing.data().putInt(0,99);
            pre.copyBuffer(device,0,host,0,16);
            pre.copyBuffer(upload,0,device,8,8);
            var callbacks=new ArrayList<Integer>();
            pre.onCompleted(()->{
                callbacks.add(1);
                renderer.pollCompleted();
            });
            pre.submit();
            current.set(next);
            crossing.close();
            assertThrows(IllegalArgumentException.class,()->next.copyBuffer(device,0,device,2,8));
            next.copyBuffer(device,0,device,12,4);
            next.writeBuffer(host,0,MemorySegment.ofArray(new int[]{123,124}));
            next.copyBuffer(device,0,host,0,16);
            next.writeBuffer(host,12,MemorySegment.ofArray(new int[]{55}));
            next.onCompleted(()->callbacks.add(2));
            next.submit();
            assertTrue(done.awaitCompletion(-1));
            assertEquals(java.util.List.of(1,2),callbacks);
            var data=visible.data().asIntBuffer();
            assertEquals(10,data.get(0));
            assertEquals(99,data.get(1));
            assertEquals(77,data.get(2));
            assertEquals(55,data.get(3));
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void writableSliceSurvivesTwoEncoderSubmissionsAndTransientReuse(NativeEnvironment.Backend backend) throws Exception {
        try (var env = new NativeEnvironment(NativeTestSupport.locate(directory), backend, NativeTestSupport.recordingLogger(new ArrayList<>()));
             var runtime = env.createRuntime(); var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var device = new dev.raynium.client.renderpearl.RayniumGpuDevice(runtime, renderer, true);
             var gpu = (NativeBuffer)device.createBuffer(null, GpuBuffer.USAGE_MAP_WRITE | GpuBuffer.USAGE_COPY_DST | GpuBuffer.USAGE_COPY_SRC, 8);
             var host = (NativeBuffer)device.createBuffer(null, GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, 8);
             var mapped = host.map(true, false)) {
            var encoder = device.createCommandEncoder();
            try (var slice = gpu.map(0, 4, false, true)) {
                slice.data().putInt(0, 33);
                var first = dev.raynium.client.renderpearl.TerrainTestPass.batch(device);
                first.writeBuffer(gpu, 0, MemorySegment.ofArray(new int[]{11, 22}));
                for (int index = 0; index < 2; ++index) {
                    var temporary = encoder.transientMemory().allocateCpu(32, 4, 32, 1);
                    temporary.putInt(0, index);
                    encoder.submit();
                }
                assertEquals(33, slice.data().getInt(0));
            }
            var last = dev.raynium.client.renderpearl.TerrainTestPass.batch(device);
            last.copyBuffer(gpu, 0, host, 0, 8);
            try (var fence = encoder.createFence()) {
                encoder.submit();
                assertTrue(fence.awaitCompletion(-1));
            }
            assertEquals(33, mapped.data().getInt(0));
            assertEquals(22, mapped.data().getInt(4));
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void repacksOddDepthRowsAndUnalignedColorTransfers(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var tape=new CommandTape(renderer); var done=tape.createFence();
            var depth=renderer.createTexture(3,2,1,1,NativeTexture.Format.D16_UNORM,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var clone=renderer.createTexture(3,2,1,1,NativeTexture.Format.D16_UNORM,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var color=renderer.createTexture(63,2,1,1,NativeTexture.Format.RGBA8,NativeTexture.READBACK);
            var host=renderer.createBuffer(1|8,1024,null,()->tape); var open=host.map(true,false);
            var tightDepth=renderer.createBuffer(8|16,12,null,()->tape);
            var gpu=renderer.createBuffer(8|16,508,null,()->tape);
            var pixels=renderer.createBuffer(2|16,508,null,()->tape); var mapped=pixels.map(false,true)) {
            for(int i=0;i<126;++i) mapped.data().putInt(4+4*i,0xff000000|i);
            var depthRegion=new Region(depth,0,0,0,0,3,2);
            var cloneRegion=new Region(clone,0,0,0,0,3,2);
            tape.clearDepth(depthRegion,1);
            tape.clearDepth(new Region(depth,0,0,0,0,3,1),0);
            tape.copyTexture(depthRegion,cloneRegion);
            assertThrows(UnsupportedOperationException.class,()->tape.textureToBuffer(new Region(depth,0,0,0,0,2,2),host,0,null));
            tape.textureToBuffer(cloneRegion,host,0,null);
            tape.textureToBuffer(cloneRegion,tightDepth,0,null);
            tape.copyBuffer(tightDepth,0,host,768,12);
            var colorRegion=new Region(color,0,0,0,0,63,2);
            tape.bufferToTexture(pixels,4,252,colorRegion);
            tape.textureToBuffer(colorRegion,gpu,4,null);
            tape.copyBuffer(gpu,4,host,24,504);
            tape.writeBuffer(gpu,4,MemorySegment.ofArray(new int[]{0x11223344}));
            tape.bufferToTexture(gpu,4,252,colorRegion);
            tape.textureToBuffer(colorRegion,host,16,null);
            tape.submit();
            assertTrue(done.awaitCompletion(-1));
            for(int i=0;i<6;++i) {
                short expected=(short)(i<3?0:-1);
                assertEquals(expected,open.data().getShort(i*2));
                assertEquals(expected,open.data().getShort(768+i*2));
            }
            assertEquals(0x11223344,open.data().getInt(16));
            for(int i=1;i<126;++i) assertEquals(0xff000000|i,open.data().getInt(16+4*i));
            assertEquals(0xff000000|125,open.data().getInt(24+4*125));
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void firstPartialAccessInitializesAllMipsAndLayers(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var color=renderer.createTexture(4,4,2,2,NativeTexture.Format.RGBA8,NativeTexture.READBACK);
            var depth=renderer.createTexture(3,2,1,1,NativeTexture.Format.D16_UNORM,NativeTexture.READBACK);
            var tape=new CommandTape(renderer); var fence=tape.createFence();
            var target=renderer.createBuffer(8|1,12,null,()->tape); var mapped=target.map(true,false)) {
            tape.uploadTexture(new Region(color,1,1,1,1,1,1),4,MemorySegment.ofArray(new int[]{0x12345678}));
            tape.captureColor(new Region(color,0,0,0,0,4,4),1);
            tape.captureColor(new Region(color,1,1,0,0,2,2),2);
            tape.textureToBuffer(new Region(depth,0,0,0,0,3,2),target,0,null);
            tape.submit();
            assertTrue(fence.awaitCompletion(-1));
            assertArrayEquals(new byte[64],renderer.pollCapture(1).orElseThrow().rgba());
            var expected=new int[]{0,0,0,0x12345678};
            var actual=renderer.pollCapture(2).orElseThrow().rgba();
            assertArrayEquals(expected,MemorySegment.ofArray(actual).toArray(ValueLayout.JAVA_INT_UNALIGNED));
            for(int i=0;i<12;++i) assertEquals(0,mapped.data().get(i));
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void publishesStd140FloatsAndPartiallyInitializedGlyphTextures(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var tape=new CommandTape(renderer); var done=tape.createFence();
            var upload=renderer.createBuffer(GpuBuffer.USAGE_MAP_WRITE|GpuBuffer.USAGE_COPY_SRC,8,null,()->tape);
            var device=renderer.createBuffer(GpuBuffer.USAGE_MAP_WRITE|GpuBuffer.USAGE_COPY_DST|GpuBuffer.USAGE_COPY_SRC,16,null,()->tape);
            var target=renderer.createBuffer(GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,12,null,()->tape);
            var open=upload.map(false,true); var cpu=target.map(true,false);
            var glyph=renderer.createTexture("glyph",7,GpuFormat.R8_UNORM,256,256,1,1);
            var pixels=renderer.createBuffer(GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,256*256,null,()->tape);
            var mapped=pixels.map(true,false)) {
            Std140Builder.intoBuffer(open.data()).putFloat(1f);
            try(var local=device.map(4,4,false,true)) { Std140Builder.intoBuffer(local.data()).putFloat(1f); }
            tape.copyBuffer(upload,0,target,0,4);
            tape.copyBuffer(device,4,target,4,4);
            tape.writeBuffer(target,8,MemorySegment.ofArray(new byte[]{0x11,0x22,0x33,0x44}));
            tape.uploadTexture(new Region(glyph,0,0,19,21,2,2),2,MemorySegment.ofArray(new byte[]{1,2,3,4}));
            tape.textureToBuffer(new Region(glyph,0,0,0,0,256,256),pixels,0,null);
            tape.submit();
            assertTrue(done.awaitCompletion(-1));
            var raw=new byte[8];
            cpu.data().get(0,raw);
            assertArrayEquals(new byte[]{0,0,(byte)0x80,0x3f,0,0,(byte)0x80,0x3f},raw);
            assertEquals(0x44332211,cpu.data().getInt(8));
            var expected=new byte[256*256];
            expected[21*256+19]=1; expected[21*256+20]=2; expected[22*256+19]=3; expected[22*256+20]=4;
            var actual=new byte[expected.length];
            mapped.data().get(0,actual);
            assertArrayEquals(expected,actual);
        }
    }

    private static void checkPixels(byte[] pixels,int... row) {
        assertEquals(8*row.length,pixels.length);
        var bytes=java.nio.ByteBuffer.wrap(pixels).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        for(int y=0;y<2;++y) for(int expected:row) assertEquals(expected,bytes.getInt());
    }
}
