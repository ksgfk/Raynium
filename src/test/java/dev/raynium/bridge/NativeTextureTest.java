package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.lwjgl.sdl.SDLVideo;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeTextureTest {
    @TempDir Path gameDirectory;
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void arrayAndCubeMipUploadsRetainDeletedOwners(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE)) {
            assertTrue(SDLVideo.SDL_HideWindow(window.handle)); window.pump();
            for(int n=1;n<=6;n++) {
                for(int flags:new int[]{0,NativeTexture.CUBE,NativeTexture.ATTACHMENT|NativeTexture.READBACK}) {
                    int layers=(flags&NativeTexture.CUBE)!=0?6:3;
                    for(var format:List.of(NativeTexture.Format.RGBA8,NativeTexture.Format.R8,NativeTexture.Format.RGBA16F,NativeTexture.Format.RGBA32F)) {
                        try(var texture=renderer.createTexture(8,8,4,layers,format,flags)) {
                            for(int mip=0;mip<4;mip++) for(int layer=0;layer<layers;layer++) {
                                int size=8>>mip,pitch=size*format.bytesPerPixel()+16;
                                byte[] pixels=new byte[pitch*size]; Arrays.fill(pixels,(byte)(layer+n));
                                texture.uploadLayer(mip,layer,0,0,size,size,pitch,MemorySegment.ofArray(pixels));
                            }
                            assertThrows(IllegalStateException.class,()->texture.uploadLayer(0,layers,0,0,1,1,16,MemorySegment.ofArray(new byte[16])));
                        }
                    }
                }
                assertEquals(1,NativeTestSupport.blit(renderer,chain));
            }
            try(var depth=renderer.createTexture(8,8,1,1,NativeTexture.Format.DEPTH32F,NativeTexture.ATTACHMENT|NativeTexture.READBACK)) {
                assertThrows(IllegalStateException.class,()->depth.upload(0,0,0,8,8,new byte[256]));
            }
            assertThrows(IllegalStateException.class,()->renderer.createTexture(8,4,1,6,NativeTexture.Format.RGBA8,NativeTexture.CUBE));
            assertThrows(IllegalStateException.class,()->renderer.createTexture(8,8,1,5,NativeTexture.Format.RGBA8,NativeTexture.CUBE));
            assertThrows(IllegalStateException.class,()->renderer.createTexture(8,8,1,0,NativeTexture.Format.RGBA8));
        }
    }
}
