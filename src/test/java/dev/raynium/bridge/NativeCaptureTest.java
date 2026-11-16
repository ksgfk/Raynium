package dev.raynium.bridge;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.locks.LockSupport;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.lwjgl.sdl.SDLVideo;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeCaptureTest {
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void capturesRequestedTracyThumbnailAfterGuiAcrossResize(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,641,359,NativeSwapchain.PresentMode.IMMEDIATE)) {
            for(int n=0;n<4;n++) {
                int width=n<2?641:199,height=n<2?359:113;
                if(n==2) chain.resize(width,height,NativeSwapchain.PresentMode.IMMEDIATE);
                var frame=NativeTestSupport.solidFrame(n+1); frame.clearColor=new float[]{0,0,1,1}; frame.guiWidth=width; frame.guiHeight=height;
                var vertices=ByteBuffer.allocate(96).order(ByteOrder.nativeOrder());
                for(float[] point:new float[][]{{0,0},{0,height/2f},{width/2f,height/2f},{width/2f,0}})
                    vertices.putFloat(point[0]).putFloat(point[1]).putFloat(0).putFloat(0).putFloat(0).putInt(0xff0000ff);
                frame.guiVertices=vertices.array(); frame.guiDraws=List.of(new NativeRenderer.GuiDraw(0,0,0,0,4,0,0,width,height));
                renderer.requestCapture(9500+n,NativeRenderer.CAPTURE_TRACY,0,0,0);
                renderer.requestCapture(9600+n,NativeRenderer.CAPTURE_TRACY,0,0,0);
                renderer.present(chain,frame);
                var capture=awaitCapture(renderer,9500+n);
                assertEquals(n<2?320:196,capture.width()); assertEquals(n<2?176:112,capture.height());
                pixel(capture,10,10,255,0,0,255);
                pixel(capture,capture.width()-10,10,0,0,255,255);
                pixel(capture,10,capture.height()-10,0,0,255,255);
                assertArrayEquals(capture.rgba(),awaitCapture(renderer,9600+n).rgba());
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rendersSixOffscreenFacesWithoutResizingPresentation(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,127,65,NativeSwapchain.PresentMode.IMMEDIATE)) {
            for(int face=0;face<6;face++) {
                var frame=NativeTestSupport.solidFrame(face+1); frame.renderWidth=257; frame.renderHeight=257; frame.flags=NativeRenderer.WORLD_ONLY;
                frame.clearColor=new float[]{face/5.f,1-face/5.f,0,1};
                assertEquals(NativeSwapchain.Presentation.SKIPPED,renderer.present(chain,frame));
                renderer.requestCapture(9000+face,NativeRenderer.CAPTURE_LAST,0,0,0);
            }
            var frame=NativeTestSupport.solidFrame(7); frame.flags=NativeRenderer.CAPTURE;
            renderer.present(chain,frame);
            for(int face=0;face<6;face++) {
                var capture=awaitCapture(renderer,9000+face); assertEquals(257,capture.width()); assertEquals(257,capture.height());
                pixel(capture,128,128,face*51,255-face*51,0,255);
            }
            var capture=awaitCapture(renderer,7); assertEquals(127,capture.width()); assertEquals(65,capture.height());
        }
    }
    @TempDir Path gameDirectory;
    private static NativeRenderer.Capture awaitCapture(NativeRenderer renderer,long sequence) {
        long deadline=System.nanoTime()+Duration.ofSeconds(5).toNanos();
        do {
            var result=renderer.pollCapture(sequence);
            if(result.isPresent()) return result.get();
            LockSupport.parkNanos(1_000_000);
        } while(System.nanoTime()<deadline);
        throw new AssertionError("Capture did not complete: "+sequence);
    }
    private static void pixel(NativeRenderer.Capture capture,int x,int y,int r,int g,int b,int a) {
        int offset=(y*capture.width()+x)*4;
        int[] expected={r,g,b,a};
        for(int c=0;c<4;c++) assertEquals(expected[c],capture.rgba()[offset+c]&255,1,"pixel "+x+","+y+" channel "+c);
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void capturesDeletedTextureMipLayerAndWorldBeforePostAndGui(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,127,65,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(4,4,2,2,NativeTexture.Format.RGBA8,NativeTexture.READBACK)) {
            byte[] image=new byte[16];
            for(int i=0;i<4;i++) { image[i*4]=20; image[i*4+1]=40; image[i*4+2]=60; image[i*4+3]=100; }
            texture.uploadLayer(1,1,0,0,2,2,8,java.lang.foreign.MemorySegment.ofArray(image));
            renderer.requestCapture(8001,NativeRenderer.CAPTURE_TEXTURE,texture.id(),1,1);
            assertTrue(renderer.pollCapture(8001).isEmpty()); texture.close();
            renderer.requestCapture(8002,NativeRenderer.CAPTURE_WORLD,0,0,0);
            renderer.requestCapture(8003,NativeRenderer.CAPTURE_FINAL,0,0,0);
            var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0);
            int globals=buffer.scene().copy(java.lang.foreign.MemorySegment.ofArray(new byte[256])); buffer.scene().effect(4,0,globals);
            buffer.beginGui(0,0);
            for(float[] point:new float[][]{{0,0},{0,32},{64,32},{64,0}}) buffer.guiVertex(point[0],point[1],0,0,0,0xffff0000);
            buffer.guiDraw(0,0,0,0,4,0,0,127,65);
            var frame=NativeTestSupport.solidFrame(1); frame.clearColor=new float[]{1,0,0,1}; frame.guiWidth=127; frame.guiHeight=65;
            renderer.presentPrepared(chain,frame);
            var mip=awaitCapture(renderer,8001); assertEquals(2,mip.width()); assertEquals(2,mip.height()); pixel(mip,1,1,20,40,60,100);
            pixel(awaitCapture(renderer,8002),10,10,255,0,0,255);
            var output=awaitCapture(renderer,8003); pixel(output,10,10,0,0,255,255); pixel(output,100,50,51,204,204,255);
            renderer.requestCapture(8004,NativeRenderer.CAPTURE_LAST,0,0,0);
            chain.resize(63,33,NativeSwapchain.PresentMode.IMMEDIATE); renderer.present(chain,NativeTestSupport.solidFrame(2));
            var previous=awaitCapture(renderer,8004); assertEquals(127,previous.width()); pixel(previous,10,10,0,0,255,255);
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void closesWithUnreadCaptureAndWithAllocatedButUnsubmittedReadback(NativeEnvironment.Backend backend) throws Exception {
        for(boolean fail:new boolean[]{false,true}) {
            try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
                var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
                var renderer=runtime.createRenderer(NativeTestSupport.shaders());
                var chain=renderer.createSwapchain(window.hwnd,127,65,NativeSwapchain.PresentMode.IMMEDIATE)) {
                var first=NativeTestSupport.solidFrame(1); first.flags=NativeRenderer.CAPTURE;
                renderer.present(chain,first);
                if(fail) {
                    var invalid=NativeTestSupport.solidFrame(2); invalid.flags=NativeRenderer.CAPTURE;
                    invalid.guiVertices=new byte[96]; invalid.guiWidth=Float.NaN; invalid.guiHeight=65;
                    invalid.guiDraws=List.of(new NativeRenderer.GuiDraw(0,0,0,0,4,0,0,127,65));
                    assertThrows(IllegalStateException.class,()->renderer.present(chain,invalid));
                }
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void previousCaptureSurvivesHiddenSlotReuseAndResize(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,321,179,NativeSwapchain.PresentMode.IMMEDIATE)) {
            var red=NativeTestSupport.solidFrame(1); red.clearColor=new float[]{1,0,0,1}; renderer.present(chain,red);
            assertTrue(SDLVideo.SDL_HideWindow(window.handle)); window.pump();
            renderer.present(chain,NativeTestSupport.solidFrame(2));
            var green=NativeTestSupport.solidFrame(3); green.clearColor=new float[]{0,1,0,1};
            green.flags=NativeRenderer.CAPTURE|NativeRenderer.CAPTURE_PREVIOUS;
            renderer.present(chain,green);
            var previous=awaitCapture(renderer,3); pixel(previous,5,5,255,0,0,255);
            chain.resize(199,113,NativeSwapchain.PresentMode.IMMEDIATE);
            var blue=NativeTestSupport.solidFrame(4); blue.clearColor=new float[]{0,0,1,1};
            blue.flags=NativeRenderer.CAPTURE|NativeRenderer.CAPTURE_PREVIOUS;
            renderer.present(chain,blue);
            var resized=awaitCapture(renderer,4);
            assertEquals(321,resized.width()); assertEquals(179,resized.height()); pixel(resized,5,5,0,255,0,255);
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void capturesExactColorChannelsOrientationAndHiddenFramesAcrossResize(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,641,359,NativeSwapchain.PresentMode.IMMEDIATE)) {
            for(int n=1;n<=6;n++) {
                int width=n<4?641:319,height=n<4?359:181;
                if(n==3) { assertTrue(SDLVideo.SDL_HideWindow(window.handle)); window.pump(); }
                if(n==4) chain.resize(width,height,NativeSwapchain.PresentMode.IMMEDIATE);
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE;
                frame.clearColor=new float[]{.2f,.4f,.6f,1}; frame.guiWidth=width; frame.guiHeight=height;
                var vertices=ByteBuffer.allocate(96).order(ByteOrder.nativeOrder());
                for(float[] point:new float[][]{{10,10},{10,50},{50,50},{50,10}})
                    vertices.putFloat(point[0]).putFloat(point[1]).putFloat(0).putFloat(0).putFloat(0).putInt(0xff0000ff);
                frame.guiVertices=vertices.array(); frame.guiDraws=List.of(new NativeRenderer.GuiDraw(0,0,0,0,4,0,0,width,height));
                renderer.present(chain,frame);
                var capture=awaitCapture(renderer,n);
                assertEquals(width,capture.width()); assertEquals(height,capture.height()); assertEquals(width*height*4,capture.rgba().length);
                pixel(capture,2,2,51,102,153,255); pixel(capture,20,20,255,0,0,255);
                pixel(capture,20,height-20,51,102,153,255);
                long consumed=n;
                assertThrows(IllegalStateException.class,()->renderer.pollCapture(consumed));
            }
        }
    }
}
