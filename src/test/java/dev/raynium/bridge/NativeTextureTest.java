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
    static byte[] quad() {
        var b=ByteBuffer.allocate(112).order(ByteOrder.nativeOrder());
        for(float[] point:new float[][]{{-.75f,-.75f},{-.75f,.75f},{.75f,.75f},{.75f,-.75f}}) {
            b.putFloat(point[0]).putFloat(point[1]).putFloat(.5f).putInt(0xffffffff);
            b.putFloat((point[0]+1)/2).putFloat((point[1]+1)/2).putInt(0x00f000f0);
        }
        return b.array();
    }
    static byte[] gui() {
        var b=ByteBuffer.allocate(96).order(ByteOrder.nativeOrder());
        for(float[] point:new float[][]{{10,10},{10,50},{50,50},{50,10}})
            b.putFloat(point[0]).putFloat(point[1]).putFloat(0).putFloat(0).putFloat(0).putInt(0xffffffff);
        return b.array();
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void mipRegionUploadsAndInFlightTerrainGuiSurviveSwapchainChanges(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime()) {
            for(var mode:List.of(TerrainSubmission.DIRECT,TerrainSubmission.INDIRECT)) for(var format:NativeTexture.Format.values()) {
                try(var renderer=runtime.createRenderer(NativeTestSupport.shaders(),mode);
                    var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE);
                    var atlas=renderer.createTexture(8,8,4,format);
                    var light=renderer.createTexture(16,16,1,NativeTexture.Format.RGBA8)) {
                    for(int mip=0;mip<4;mip++) {
                        int size=8>>mip; byte[] bytes=new byte[size*size*format.bytesPerPixel()]; Arrays.fill(bytes,(byte)255);
                        atlas.upload(mip,0,0,size,size,bytes); Arrays.fill(bytes,(byte)0);
                    }
                    byte[] lighting=new byte[16*16*4]; Arrays.fill(lighting,(byte)255); light.upload(0,0,0,16,16,lighting);
                    int pitch=4*format.bytesPerPixel()+8; byte[] padded=new byte[pitch*3+4*format.bytesPerPixel()]; Arrays.fill(padded,(byte)200);
                    atlas.upload(0,2,2,4,4,pitch,MemorySegment.ofArray(padded));
                    assertThrows(IllegalStateException.class,()->atlas.upload(4,0,0,1,1,new byte[4]));
                    assertThrows(IllegalStateException.class,()->atlas.upload(0,0,0,4,4,new byte[1]));
                    assertTrue(SDLVideo.SDL_HideWindow(window.handle)); window.pump();
                    for(int n=0;n<24;n++) {
                        byte[] update=new byte[4*format.bytesPerPixel()]; Arrays.fill(update,(byte)(180+n));
                        atlas.upload(0,0,0,2,2,update);
                        atlas.upload(0,1,1,2,2,update);
                        var frame=NativeTestSupport.solidFrame(n+1); frame.flags=NativeRenderer.PROFILE|(n%2==0?NativeRenderer.IMPROVED_TRANSPARENCY:0);
                        frame.terrainAlbedoTexture=atlas.id(); frame.terrainLightTexture=light.id();
                        if(n==0 || n==12) {
                            frame.meshes=List.of(new NativeRenderer.Mesh(1,quad(),4,0),new NativeRenderer.Mesh(2,quad(),4,1),new NativeRenderer.Mesh(3,quad(),4,2));
                        }
                        frame.epoch=n<12?1:2;
                        if(n==0 || n==12 || n%3==0) frame.indices=List.of(new NativeRenderer.Index(3,n%2==0?new int[]{0,1,2,2,3,0}:new int[]{2,3,0,0,1,2}));
                        if(n==4) frame.meshes=List.of(new NativeRenderer.Mesh(4,new byte[70000*28],70000,0));
                        if(n==6) frame.removedMeshes=new long[]{4};
                        frame.instances=List.of(new NativeRenderer.Instance(0,0,0,1),new NativeRenderer.Instance(0,0,0,.8f));
                        frame.visible=List.of(new NativeRenderer.Visible(1,-1,0,0,2),new NativeRenderer.Visible(2,-1,1,0,1),new NativeRenderer.Visible(3,-1,2,1,1));
                        frame.guiWidth=640; frame.guiHeight=360; frame.guiVertices=gui();
                        var draws=new ArrayList<NativeRenderer.GuiDraw>();
                        for(int pipeline=0;pipeline<12;pipeline++) draws.add(new NativeRenderer.GuiDraw(pipeline,pipeline<=2?0:atlas.id(),12,0,4,0,0,640,360));
                        frame.guiDraws=draws;
                        if(n==1) { assertTrue(SDLVideo.SDL_ShowWindow(window.handle)); window.pump(); }
                        if(n==8) chain.resize(640,360,NativeSwapchain.PresentMode.FIFO);
                        var result=renderer.present(chain,frame);
                        assertEquals(n==0?NativeSwapchain.Presentation.SKIPPED:NativeSwapchain.Presentation.PRESENTED,result);
                    }
                    int destroyed=atlas.id(); atlas.close();
                    var invalid=NativeTestSupport.solidFrame(25); invalid.epoch=2;
                    invalid.terrainAlbedoTexture=destroyed; invalid.terrainLightTexture=light.id();
                    invalid.meshes=List.of(new NativeRenderer.Mesh(3,quad(),4,2));
                    invalid.instances=List.of(new NativeRenderer.Instance(0,0,0,1)); invalid.visible=List.of(new NativeRenderer.Visible(1,-1,0,0,1));
                    var failure=assertThrows(IllegalStateException.class,()->renderer.present(chain,invalid));
                    assertTrue(failure.getMessage().contains("present frame"));
                }
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void replacedMeshesAndDeletedMaterialTexturesSurviveSlotReuse(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime()) {
            for(var mode:List.of(TerrainSubmission.DIRECT,TerrainSubmission.INDIRECT)) {
                try(var renderer=runtime.createRenderer(NativeTestSupport.shaders(),mode);
                    var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE);
                    var light=renderer.createTexture(16,16,1,NativeTexture.Format.RGBA8)) {
                    byte[] lighting=new byte[16*16*4]; Arrays.fill(lighting,(byte)255); light.upload(0,0,0,16,16,lighting);
                    for(int n=1;n<=12;n++) {
                        try(var atlas=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8)) {
                            atlas.upload(0,0,0,1,1,new byte[]{-1,-1,-1,-1});
                            var frame=NativeTestSupport.solidFrame(n);
                            frame.flags=n%2==0?NativeRenderer.IMPROVED_TRANSPARENCY:0;
                            frame.terrainAlbedoTexture=atlas.id(); frame.terrainLightTexture=light.id();
                            if(n==6 || n==12) {
                                frame.removedMeshes=new long[]{3};
                            } else {
                                frame.meshes=List.of(new NativeRenderer.Mesh(3,quad(),4,2));
                                if(n==1 || n==5 || n==7 || n==10)
                                    frame.indices=List.of(new NativeRenderer.Index(3,new int[]{0,1,2,2,3,0}));
                                frame.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                                frame.visible=List.of(new NativeRenderer.Visible(3,-1,0,0,1));
                                frame.guiWidth=640; frame.guiHeight=360; frame.guiVertices=gui();
                                frame.guiDraws=List.of(new NativeRenderer.GuiDraw(3,atlas.id(),12,0,4,0,0,640,360));
                            }
                            assertEquals(NativeSwapchain.Presentation.PRESENTED,renderer.present(chain,frame));
                        }
                    }
                }
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void uploadOnlyFramesRetainDestroyedTargetsAcrossArenaGrowth(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE)) {
            assertTrue(SDLVideo.SDL_HideWindow(window.handle)); window.pump();
            byte[] pixels=new byte[2048*1024*4]; Arrays.fill(pixels,(byte)255);
            for(int frame=1;frame<=6;frame++) {
                try(var small=renderer.createTexture(8,8,1,NativeTexture.Format.RGBA8);
                    var large=renderer.createTexture(2048,1024,1,NativeTexture.Format.RGBA8)) {
                    small.upload(0,0,0,8,8,new byte[256]);
                    large.upload(0,0,0,2048,1024,pixels);
                    large.upload(0,1,1,2,2,new byte[16]);
                }
                assertEquals(NativeSwapchain.Presentation.SKIPPED,renderer.present(chain,NativeTestSupport.solidFrame(frame)));
            }
            assertTrue(SDLVideo.SDL_ShowWindow(window.handle)); window.pump();
            chain.resize(1281,721,NativeSwapchain.PresentMode.IMMEDIATE);
            assertEquals(NativeSwapchain.Presentation.PRESENTED,renderer.present(chain,NativeTestSupport.solidFrame(7)));
        }
    }
}
