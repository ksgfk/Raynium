package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.concurrent.locks.LockSupport;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeSceneTest {
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void postMosaicPreservesBottomAnchoredTexelsAtNonPowerOfTwoExtent(NativeEnvironment.Backend backend) throws Exception {
        int width=170,height=98;
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,width,height,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(width,height,1,NativeTexture.Format.RGBA8); var arena=Arena.ofConfined()) {
            int[] input=new int[width*height];
            for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
                int gray=(x*37+y*71)&255; input[y*width+x]=0xff000000|gray*0x010101;
            }
            texture.upload(0,0,0,width,height,width*4,MemorySegment.ofArray(input));
            var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256);
            for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
            var vertices=arena.allocate(96,4); float[][] xy={{-1,-1},{1,-1},{1,1},{-1,1}};
            for(int i=0;i<4;i++) { long at=i*24L; vertices.set(JAVA_FLOAT,at,xy[i][0]); vertices.set(JAVA_FLOAT,at+4,xy[i][1]);
                vertices.set(JAVA_FLOAT,at+8,.5f); vertices.set(JAVA_FLOAT,at+12,(xy[i][0]+1)/2);
                vertices.set(JAVA_FLOAT,at+16,(1-xy[i][1])/2); vertices.set(JAVA_INT,at+20,-1); }
            byte[] original=null;
            for(int n=1;n<=3;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                int cb=scene.copy(constants),vb=scene.borrow(vertices),first=scene.bindingCount();
                scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.texture(0,texture.id(),32);
                scene.draw(52,NativeSceneFrame.SOLID,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,width,height);
                if(n>1) scene.effect(2,0,scene.copy(arena.allocate(256,256)));
                renderer.presentPrepared(chain,frame); byte[] pixels=awaitCapture(renderer,n).rgba();
                if(n==1) { original=pixels; continue; }
                for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
                    int anchorX=(x/4)*4,anchorY=height-1-((height-1-y)/4)*4,at=(y*width+x)*4;
                    // Mosaic UVs lie exactly on texel edges; API sampling precision can select either neighbor.
                    boolean matched=false;
                    for(int dx=-1;dx<=0;dx++) for(int dy=0;dy<=1;dy++) {
                        int source=(Math.min(height-1,anchorY+dy)*width+Math.max(0,anchorX+dx))*4;
                        int gray=(int)Math.round(Math.min(255,(original[source]&255)*1.328));
                        int green=(int)Math.round(Math.min(1,Math.floor(gray/255.0*16)/16*1.205)*255);
                        matched|=Math.abs(green-(pixels[at+1]&255))<=1;
                    }
                    assertTrue(matched,"Mosaic sampled outside its texel boundary: x="+x+" y="+y+" frame="+n);
                    assertEquals(0,pixels[at]&255); assertEquals(0,pixels[at+2]&255); assertEquals(255,pixels[at+3]&255);
                }
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void interleavesSceneGuiDrawsWithFlatGuiAndBlurWithoutLosingOrder(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8); var arena=Arena.ofConfined()) {
            texture.upload(0,0,0,1,1,4,MemorySegment.ofArray(new int[]{-1}));
            for(int n=1;n<=3;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE|NativeRenderer.MEASURE|(n==2?NativeRenderer.PROFILE:0); frame.guiWidth=128; frame.guiHeight=64;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); buffer.beginGui(0,0); var scene=buffer.scene();
                var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256); constants.set(JAVA_FLOAT,276,-1);
                for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
                int cb=scene.copy(constants); guiRect(buffer,0,0,128,64,0xff0000ff);
                for(int part=0;part<2;part++) {
                    var vertices=arena.allocate(96,4); float left=part==0?-1:.5f,right=part==0?0:1;
                    float[][] xy={{left,-1},{right,-1},{right,1},{left,1}};
                    for(int i=0;i<4;i++) { long at=i*24L; vertices.set(JAVA_FLOAT,at,xy[i][0]); vertices.set(JAVA_FLOAT,at+4,xy[i][1]);
                        vertices.set(JAVA_FLOAT,at+8,.5f); vertices.set(JAVA_INT,at+20,part==0?0xff00ff00:0xffff0000); }
                    int vb=scene.borrow(vertices),first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.texture(0,texture.id(),32);
                    int draw=scene.draw(52,NativeSceneFrame.GUI,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                    buffer.guiDraw(12,draw,0,buffer.guiVertexCount(),0,0,0,128,64);
                    if(part==0) { scene.guiBlurSplit(buffer.guiDrawCount()); scene.effect(1,1,scene.copy(arena.allocate(256,256))); guiRect(buffer,48,0,112,64,0xff00ffff); }
                }
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba();
                if(n>=3) { assertTrue(chain.measurement(0)>0); assertTrue(chain.measurement(1)>0); }
                assertTrue(chain.measurement(3)>0); assertTrue(chain.measurement(4)>0);
                assertTrue(chain.measurement(8)>0); assertTrue(chain.measurement(9)>0);
                assertTrue(chain.measurement(12)>=2); assertTrue(chain.measurement(13)>=2); assertTrue(chain.measurement(14)>0);
                for(int[] point:new int[][]{{16,0,255,0},{80,255,255,0},{120,0,0,255}})
                    for(int c=0;c<3;c++) assertEquals(point[c+1],pixels[(32*128+point[0])*4+c]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void samplesMipViewsInSceneAndGuiAcrossSlotsUpdatesAndDeletion(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(8,8,4,NativeTexture.Format.RGBA8); var arena=Arena.ofConfined()) {
            int[] colors={0xff0000ff,0xff00ff00,0xffff0000,0xffffffff};
            for(int mip=0;mip<4;mip++) {
                int size=8>>mip; int[] pixels=new int[size*size]; java.util.Arrays.fill(pixels,colors[mip]);
                texture.upload(mip,0,0,size,size,size*4,MemorySegment.ofArray(pixels));
            }
            var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256);
            for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
            var vertices=arena.allocate(96,4); float[][] xy={{-1,-1},{0,-1},{0,1},{-1,1}};
            for(int i=0;i<4;i++) { long at=i*24L; vertices.set(JAVA_FLOAT,at,xy[i][0]); vertices.set(JAVA_FLOAT,at+4,xy[i][1]);
                vertices.set(JAVA_FLOAT,at+8,.5f); vertices.set(JAVA_FLOAT,at+12,.5f); vertices.set(JAVA_FLOAT,at+16,.5f); vertices.set(JAVA_INT,at+20,-1); }
            for(int n=1;n<=6;n++) {
                if(n==5) { int[] pixels=new int[16]; java.util.Arrays.fill(pixels,0xff00ffff); texture.upload(1,0,0,4,4,16,MemorySegment.ofArray(pixels)); colors[1]=0xff00ffff; }
                int mip=n==3?2:1;
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE; frame.guiWidth=128; frame.guiHeight=64;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                int cb=scene.copy(constants),vb=scene.borrow(vertices),first=scene.bindingCount();
                scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.texture(0,texture.id(),28,mip,1,1.5f,n%2==0?4:1);
                scene.draw(52,NativeSceneFrame.SOLID,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                buffer.beginGui(0,0); buffer.guiVertex(64,0,0,0,0,-1); buffer.guiVertex(64,64,0,0,1,-1);
                buffer.guiVertex(128,64,0,1,1,-1); buffer.guiVertex(128,0,0,1,0,-1);
                buffer.guiDraw(3,texture.id(),28,0,4,0,0,128,64,mip,1,1.5f,n%2==0?4:1);
                renderer.presentPrepared(chain,frame); if(n==6) texture.close();
                var pixels=awaitCapture(renderer,n).rgba();
                for(int x:new int[]{32,96}) for(int c=0;c<3;c++) assertEquals((colors[mip]>>>(c*8))&255,pixels[(32*128+x)*4+c]&255,"frame="+n+" x="+x+" channel="+c);
            }
        }
    }
    private static void guiRect(NativeFrameBuffer buffer,float x0,float y0,float x1,float y1,int color) {
        int first=buffer.guiVertexCount();
        buffer.guiVertex(x0,y0,0,0,0,color); buffer.guiVertex(x0,y1,0,0,0,color);
        buffer.guiVertex(x1,y1,0,0,0,color); buffer.guiVertex(x1,y0,0,0,0,color);
        buffer.guiDraw(0,0,0,first,4,0,0,128,64);
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void compositesColoredOutlineWithoutFillingItsInterior(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8); var arena=Arena.ofConfined()) {
            texture.upload(0,0,0,1,1,4,MemorySegment.ofArray(new int[]{-1}));
            for(int n=1;n<=3;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{0,0,0,1};
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256);
                for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
                var vertices=arena.allocate(96,4); float[][] xy={{-.5f,-.5f},{-.5f,.5f},{.5f,.5f},{.5f,-.5f}};
                for(int i=0;i<4;i++) { vertices.set(JAVA_FLOAT,i*24L,xy[i][0]); vertices.set(JAVA_FLOAT,i*24L+4,xy[i][1]);
                    vertices.set(JAVA_FLOAT,i*24L+8,.5f); vertices.set(JAVA_INT,i*24L+20,n==3?0xff00ff00:0xff0000ff); }
                int cb=scene.copy(constants),vb=scene.borrow(vertices),first=scene.bindingCount();
                scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.texture(0,texture.id(),32);
                scene.draw(169,NativeSceneFrame.OUTLINE,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba();
                int center=(32*128+64)*4,edge=(32*128+32)*4;
                assertEquals(0,pixels[center]&255); assertEquals(0,pixels[center+1]&255);
                assertTrue((pixels[edge+(n==3?1:0)]&255)>40,"Outline color must survive edge detection and blur");
                assertEquals(0,pixels[0]&255); assertEquals(255,pixels[center+3]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void appliesBuiltinChainsBeforeGuiAndPreservesBlurBoundary(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE); var arena=Arena.ofConfined()) {
            for(int n=1;n<=6;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{1,0,0,1}; frame.guiWidth=128; frame.guiHeight=64;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var globals=arena.allocate(256,256); globals.set(JAVA_FLOAT,32,128); globals.set(JAVA_FLOAT,36,64); globals.set(JAVA_INT,40,6);
                int constants=scene.copy(globals); buffer.beginGui(0,0);
                if(n<=4) scene.effect(new int[]{1,4,2,5}[n-1],0,constants);
                else {
                    frame.clearColor=new float[]{0,0,0,1};
                    guiRect(buffer,0,0,64,64,0xff0000ff);
                    scene.guiBlurSplit(1); scene.effect(1,1,constants);
                }
                guiRect(buffer,96,16,112,48,0xff00ff00);
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba(); int at=(32*128+32)*4,overlay=(32*128+100)*4;
                if(n==1) { assertEquals(255,pixels[at]&255,1); assertEquals(0,pixels[at+1]&255); }
                if(n==2) { assertEquals(51,pixels[at]&255,1); assertEquals(204,pixels[at+1]&255,1); assertEquals(204,pixels[at+2]&255,1); }
                if(n==3) { assertEquals(0,pixels[at]&255); assertEquals(115,pixels[at+1]&255,2); }
                if(n==4) assertTrue((pixels[at]&255)>0,"Spider chain must retain the scene");
                if(n>=5) { int edge=(32*128+63)*4; assertTrue((pixels[edge]&255)>40 && (pixels[edge]&255)<220,"GUI before blur must be filtered"); }
                assertEquals(0,pixels[overlay]&255); assertEquals(255,pixels[overlay+1]&255); assertEquals(0,pixels[overlay+2]&255);
                if(n>=5) assertEquals(0,pixels[(32*128+95)*4+1]&255,"GUI after blur must retain its sharp edge");
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void reflectedItemAtlasKeepsFrontFaceLighting(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var atlas=renderer.createTexture(32,32,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT);
            var white=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8); var arena=Arena.ofConfined()) {
            white.upload(0,0,0,1,1,4,MemorySegment.ofArray(new int[]{-1}));
            for(int n=1;n<=3;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE; frame.guiWidth=128; frame.guiHeight=64;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(1024,256); identity(constants,0); identity(constants,64); identity(constants,768);
                for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
                for(int at=272;at<=292;at+=4) constants.set(JAVA_FLOAT,at,Float.MAX_VALUE);
                constants.set(JAVA_FLOAT,520,1);
                var vertex=arena.allocate(8*36,4); float[][] xy={{-.8f,-.8f},{-.8f,.8f},{.8f,.8f},{.8f,-.8f}};
                for(int side=0;side<2;side++) for(int i=0;i<4;i++) {
                    long at=(side*4L+i)*36; int point=side==0?i:3-i;
                    vertex.set(JAVA_FLOAT,at,xy[point][0]); vertex.set(JAVA_FLOAT,at+4,xy[point][1]); vertex.set(JAVA_FLOAT,at+8,.5f);
                    vertex.set(JAVA_INT,at+12,side==0?0xff0000ff:0xffff0000); vertex.set(JAVA_BYTE,at+34,(byte)(side==0?127:-127));
                }
                int cb=scene.copy(constants),vb=scene.borrow(vertex),first=scene.bindingCount();
                scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.constant(3,cb,512,256); scene.constant(4,cb,768,256);
                for(int binding:new int[]{12,14,16}) scene.texture(binding,white.id(),32);
                scene.beginTarget(atlas.id(),0,0,32,32);
                scene.draw(57,NativeSceneFrame.OFFSCREEN,vb,-1,8,0,0,first,1,-1,0,0,0,0,0,32,32); scene.endTarget();
                buffer.beginGui(0,0);
                buffer.guiVertex(0,0,0,0,0,-1); buffer.guiVertex(0,64,0,0,1,-1);
                buffer.guiVertex(128,64,0,1,1,-1); buffer.guiVertex(128,0,0,1,0,-1); buffer.guiDraw(4,atlas.id(),32,0,4,0,0,128,64);
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba(); int center=(32*128+64)*4;
                assertEquals(255,pixels[center]&255); assertEquals(0,pixels[center+2]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void updatesAtlasTilesWithoutLosingCachedNeighborsAndClearsEmptyTiles(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var atlas=renderer.createTexture(64,32,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT); var arena=Arena.ofConfined()) {
            for(int n=1;n<=4;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{0,0,0,1}; frame.guiWidth=128; frame.guiHeight=64;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                if(n!=3) for(int tile=0;tile<(n==1?2:1);tile++) {
                    scene.beginTarget(atlas.id(),tile*32,0,32,32);
                    if(n!=4) {
                        var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256);
                        for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
                        var vertex=arena.allocate(64,4); float[][] xy={{-.8f,.2f},{-.8f,.8f},{.8f,.8f},{.8f,.2f}};
                        for(int i=0;i<4;i++) { vertex.set(JAVA_FLOAT,i*16L,xy[i][0]); vertex.set(JAVA_FLOAT,i*16L+4,xy[i][1]);
                            vertex.set(JAVA_FLOAT,i*16L+8,.5f); vertex.set(JAVA_INT,i*16L+12,tile==1?0xff00ff00:n==1?0xff0000ff:0xffff0000); }
                        int cb=scene.copy(constants),vb=scene.borrow(vertex),first=scene.bindingCount();
                        scene.constant(0,cb,0,256); scene.constant(1,cb,256,256);
                        scene.draw(23,NativeSceneFrame.OFFSCREEN,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,32,32);
                    }
                    scene.endTarget();
                }
                buffer.beginGui(0,0);
                buffer.guiVertex(0,0,0,0,0,-1); buffer.guiVertex(0,64,0,0,1,-1);
                buffer.guiVertex(128,64,0,1,1,-1); buffer.guiVertex(128,0,0,1,0,-1);
                buffer.guiDraw(4,atlas.id(),32,0,4,0,0,128,64);
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba();
                int left=(16*128+32)*4,right=(16*128+96)*4,bottom=(48*128+32)*4;
                assertEquals(n==1?255:0,pixels[left]&255); assertEquals(n==2 || n==3?255:0,pixels[left+2]&255);
                assertEquals(255,pixels[right+1]&255); assertEquals(0,pixels[bottom]&255); assertEquals(0,pixels[bottom+2]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void samplesSignedCloudFacesWithOffsetsAndIndependentOitDepth(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var faces=renderer.createSceneBuffer(1024); var indices=renderer.createSceneBuffer(64); var arena=Arena.ofConfined()) {
            // Encoded negative X and the high parity flag must both survive R8_SINT loads.
            faces.upload(256,MemorySegment.ofArray(new byte[]{-1,0,(byte)130}));
            indices.upload(16,MemorySegment.ofArray(new int[]{0,1,2,2,3,0}));
            for(int n=1;n<=6;n++) {
                boolean oit=n>2;
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE|(oit?NativeRenderer.IMPROVED_TRANSPARENCY:0);
                frame.clearColor=new float[]{0,0,0,1}; var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(1024,256);
                constants.set(JAVA_FLOAT,0,n<5?1:0); constants.set(JAVA_FLOAT,4,n<5?0:1); constants.set(JAVA_FLOAT,12,.5f);
                constants.set(JAVA_FLOAT,16,.8f); constants.set(JAVA_FLOAT,20,-.8f); constants.set(JAVA_FLOAT,24,.5f);
                constants.set(JAVA_FLOAT,32,1.6f); constants.set(JAVA_FLOAT,36,1.6f); constants.set(JAVA_FLOAT,40,1);
                identity(constants,256); identity(constants,320); identity(constants,768); constants.set(JAVA_FLOAT,512+36,100000);
                int cb=scene.copy(constants);
                for(int stage=0;stage<(oit?3:1);stage++) {
                    int first=scene.bindingCount(); for(int binding=0;binding<4;binding++) scene.constant(binding,cb,binding*256L,256);
                    scene.signedByteBuffer(0,faces.reference(),256,3);
                    if(oit && stage>0) scene.graphTexture(10,1);
                    if(oit && stage==2) { scene.graphTexture(2,2); scene.graphTexture(4,3); }
                    scene.draw(oit?new int[]{119,120,118}[stage]:45,oit?NativeSceneFrame.CLOUD_BOUNDS+stage:NativeSceneFrame.AFTER_TERRAIN,
                        -1,indices.reference(),4,6,4,first,1,-1,0,16,0,0,0,128,64);
                }
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba(); int center=(32*128+64)*4;
                assertEquals(n<5?102:0,pixels[center]&255,3); assertEquals(n<5?0:102,pixels[center+1]&255,3);
                assertEquals(0,pixels[0]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void persistsSceneGeometryAcrossSlotsUpdatesAndEpochs(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var vertices=renderer.createSceneBuffer(160); var indices=renderer.createSceneBuffer(64)) {
            try(var bytes=Arena.ofConfined()) {
                var data=bytes.allocate(64,4); float[][] xy={{-.8f,-.8f},{-.8f,.8f},{.8f,.8f},{.8f,-.8f}};
                for(int i=0;i<4;i++) { data.set(JAVA_FLOAT,i*16L,xy[i][0]); data.set(JAVA_FLOAT,i*16L+4,xy[i][1]);
                    data.set(JAVA_FLOAT,i*16L+8,.5f); data.set(JAVA_INT,i*16L+12,0xff0000ff); }
                vertices.upload(32,data); indices.upload(16,MemorySegment.ofArray(new int[]{0,1,2,2,3,0}));
            }
            for(int n=1;n<=4;n++) {
                if(n==3) for(int v=0;v<4;v++) vertices.upload(32+v*16L+12,MemorySegment.ofArray(new int[]{0xff00ff00}));
                var frame=NativeTestSupport.solidFrame(n); frame.epoch=n<3?1:2; frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{0,0,0,1};
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                try(var arena=Arena.ofConfined()) {
                    var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256);
                    for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,128+c*4L,1);
                    int cb=scene.copy(constants),first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,256,256);
                    scene.draw(23,NativeSceneFrame.SOLID,vertices.reference(),indices.reference(),4,6,4,first,1,-1,32,16,0,0,0,128,64);
                }
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba(); int at=(32*128+64)*4;
                assertEquals(n<3?255:0,pixels[at]&255); assertEquals(n<3?0:255,pixels[at+1]&255);
            }
            vertices.close(); indices.close();
            // Pending uploads own deleted resources even when no draw references them.
            try(var deleted=renderer.createSceneBuffer(64)) { deleted.upload(0,MemorySegment.ofArray(new int[]{1,2,3,4})); }
            for(int n=5;n<=7;n++) { renderer.frameBuffer().begin(0,0,0,0,0); var frame=NativeTestSupport.solidFrame(n); frame.epoch=2; renderer.presentPrepared(chain,frame); }
        }
    }
    @TempDir Path gameDirectory;
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void beaconFogUsesPerspectiveDistanceInsteadOfReciprocalDistance(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8); var arena=Arena.ofConfined()) {
            texture.upload(0,0,0,1,1,new byte[]{-1,-1,-1,-1});
            for(int n=1;n<=3;n++) {
                float distance=1<<n;
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(768,256); identity(constants,0); identity(constants,64);
                for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
                constants.set(JAVA_FLOAT,264,1); constants.set(JAVA_FLOAT,268,1); constants.set(JAVA_FLOAT,276,8);
                constants.set(JAVA_FLOAT,280,1000); constants.set(JAVA_FLOAT,284,2000);
                constants.set(JAVA_FLOAT,512,1); constants.set(JAVA_FLOAT,532,1);
                constants.set(JAVA_FLOAT,556,-1); constants.set(JAVA_FLOAT,568,1);
                var vertices=arena.allocate(112,4); float[][] xy={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
                for(int i=0;i<4;i++) {
                    vertices.set(JAVA_FLOAT,i*28L,xy[i][0]*distance); vertices.set(JAVA_FLOAT,i*28L+4,xy[i][1]*distance);
                    vertices.set(JAVA_FLOAT,i*28L+8,-distance); vertices.set(JAVA_INT,i*28L+12,0xff0000ff);
                }
                int cb=scene.copy(constants),vb=scene.borrow(vertices),first=scene.bindingCount();
                for(int b=0;b<3;b++) scene.constant(b,cb,b*256L,256);
                scene.texture(10,texture.id(),32);
                scene.draw(7,NativeSceneFrame.SOLID,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba(); int at=(32*128+64)*4;
                int blue=Math.round(distance/8*255);
                assertEquals(255-blue,pixels[at]&255,2); assertEquals(0,pixels[at+1]&255); assertEquals(blue,pixels[at+2]&255,2);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void compositesCloudAndDynamicOitUsingPerspectiveLinearDepth(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var faces=renderer.createSceneBuffer(512); var indices=renderer.createSceneBuffer(64); var arena=Arena.ofConfined()) {
            faces.upload(256,MemorySegment.ofArray(new byte[]{0,0,2}));
            indices.upload(16,MemorySegment.ofArray(new int[]{0,1,2,2,3,0}));
            for(int n=1;n<=4;n++) {
                boolean cloudInFront=(n&1)==0;
                float cloudDistance=cloudInFront?2:10,objectDistance=cloudInFront?10:2;
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE|NativeRenderer.IMPROVED_TRANSPARENCY;
                frame.clearColor=new float[]{0,0,0,1}; var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(1024,256);
                constants.set(JAVA_FLOAT,4,1); constants.set(JAVA_FLOAT,12,.5f);
                constants.set(JAVA_FLOAT,16,-.8f*cloudDistance); constants.set(JAVA_FLOAT,20,-.8f*cloudDistance);
                constants.set(JAVA_FLOAT,24,-cloudDistance); constants.set(JAVA_FLOAT,32,1.6f*cloudDistance);
                constants.set(JAVA_FLOAT,36,1.6f*cloudDistance); constants.set(JAVA_FLOAT,40,1);
                identity(constants,256); identity(constants,320);
                for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,384+i*4L,1);
                constants.set(JAVA_FLOAT,548,100000);
                // Reverse-Z perspective: clip.w=-view.z and device depth=1/distance.
                constants.set(JAVA_FLOAT,768,1); constants.set(JAVA_FLOAT,788,1);
                constants.set(JAVA_FLOAT,812,-1); constants.set(JAVA_FLOAT,824,1);
                var vertices=arena.allocate(64,4); float[][] xy={{-.8f,-.8f},{-.8f,.8f},{.8f,.8f},{.8f,-.8f}};
                for(int i=0;i<4;i++) {
                    vertices.set(JAVA_FLOAT,i*16L,xy[i][0]*objectDistance); vertices.set(JAVA_FLOAT,i*16L+4,xy[i][1]*objectDistance);
                    vertices.set(JAVA_FLOAT,i*16L+8,-objectDistance); vertices.set(JAVA_INT,i*16L+12,0x800000ff);
                }
                int cb=scene.copy(constants),vb=scene.borrow(vertices);
                for(int stage=0;stage<3;stage++) {
                    int first=scene.bindingCount(); scene.constant(0,cb,256,256); scene.constant(1,cb,768,256);
                    if(stage>0) scene.graphTexture(8,1);
                    if(stage==2) { scene.graphTexture(0,2); scene.graphTexture(2,3); }
                    scene.draw(new int[]{91,92,90}[stage],NativeSceneFrame.OIT_BOUNDS+stage,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                    first=scene.bindingCount(); for(int b=0;b<4;b++) scene.constant(b,cb,b*256L,256);
                    scene.signedByteBuffer(0,faces.reference(),256,3);
                    if(stage>0) scene.graphTexture(10,1);
                    if(stage==2) { scene.graphTexture(2,2); scene.graphTexture(4,3); }
                    scene.draw(new int[]{119,120,118}[stage],NativeSceneFrame.CLOUD_BOUNDS+stage,-1,indices.reference(),4,6,4,first,1,-1,0,16,0,0,0,128,64);
                }
                renderer.presentPrepared(chain,frame); var pixels=awaitCapture(renderer,n).rgba(); int center=(32*128+64)*4;
                assertEquals(cloudInFront?64:128,pixels[center]&255,3);
                assertEquals(cloudInFront?102:51,pixels[center+1]&255,3);
                assertEquals(0,pixels[center+2]&255);
            }
        }
    }
    private static void identity(MemorySegment bytes,long offset) {
        for(int i=0;i<4;i++) bytes.set(JAVA_FLOAT,offset+i*20L,1);
    }
    private static NativeRenderer.Capture awaitCapture(NativeRenderer renderer,long sequence) {
        long deadline=System.nanoTime()+Duration.ofSeconds(5).toNanos();
        while(System.nanoTime()<deadline) {
            var result=renderer.pollCapture(sequence); if(result.isPresent()) return result.get();
            LockSupport.parkNanos(1_000_000);
        }
        throw new AssertionError("Scene readback did not complete");
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void firstPersonUsesIndependentDepthWhilePreservingDrawOrder(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var arena=Arena.ofConfined()) {
            var frame=NativeTestSupport.solidFrame(1); frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{0,0,0,1};
            var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
            var constants=arena.allocate(768,256); identity(constants,0); identity(constants,64); identity(constants,512);
            for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
            constants.set(JAVA_FLOAT,272,1000); constants.set(JAVA_FLOAT,276,2000);
            constants.set(JAVA_FLOAT,280,1000); constants.set(JAVA_FLOAT,284,2000);
            var vertex=arena.allocate(128*1024,4); long vertexBase=64*1024;
            float[][] xy={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
            for(int draw=0;draw<2;draw++) for(int i=0;i<4;i++) {
                long at=vertexBase+draw*64L+i*16L; vertex.set(JAVA_FLOAT,at,xy[i][0]); vertex.set(JAVA_FLOAT,at+4,xy[i][1]);
                vertex.set(JAVA_FLOAT,at+8,draw==0?.9f:.1f); vertex.set(JAVA_INT,at+12,draw==0?0xffff0000:0xff0000ff);
            }
            int cb=scene.copy(constants),vb=scene.borrow(vertex),first=scene.bindingCount();
            scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.constant(2,cb,512,256);
            scene.draw(65,NativeSceneFrame.SOLID,vb,-1,4,0,0,first,1,-1,vertexBase,0,0,0,0,128,64);
            first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,512,256);
            scene.draw(23,NativeSceneFrame.ALWAYS_ON_TOP,vb,-1,4,0,0,first,1,-1,vertexBase,0,0,0,0,128,64);
            first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,512,256);
            scene.draw(23,NativeSceneFrame.HAND,vb,-1,4,0,0,first,1,-1,vertexBase+64,0,0,0,0,128,64);
            scene.effect(1,0,scene.copy(arena.allocate(256,256)));
            renderer.presentPrepared(chain,frame); var capture=awaitCapture(renderer,1); int center=(32*128+64)*4;
            assertEquals(255,capture.rgba()[center]&255); assertEquals(0,capture.rgba()[center+2]&255);
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void handlesOptimizedOitUniformsAndLiveTextureChanges(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
            var arena=Arena.ofConfined()) {
            for(int n=1;n<=4;n++) {
                var texel=arena.allocate(4,4); texel.set(JAVA_INT,0,n<3?0xffffffff:0xff00ff00); texture.upload(0,0,0,1,1,4,texel);
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE|NativeRenderer.IMPROVED_TRANSPARENCY;
                frame.clearColor=new float[]{0,0,0,1};
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(768,256); identity(constants,0); identity(constants,64); identity(constants,512);
                for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
                constants.set(JAVA_FLOAT,272,1000); constants.set(JAVA_FLOAT,276,2000);
                constants.set(JAVA_FLOAT,280,1000); constants.set(JAVA_FLOAT,284,2000);
                var vertex=arena.allocate(144,4); float[][] xy={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
                for(int i=0;i<4;i++) { long at=i*36L; vertex.set(JAVA_FLOAT,at,xy[i][0]); vertex.set(JAVA_FLOAT,at+4,xy[i][1]);
                    vertex.set(JAVA_FLOAT,at+8,.5f); vertex.set(JAVA_INT,at+12,0x80ffffff); }
                int cb=scene.copy(constants),vb=scene.borrow(vertex);
                for(int stage=0;stage<3;stage++) {
                    int first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.constant(2,cb,512,256);
                    scene.texture(10,texture.id(),32);
                    if(stage>0) scene.graphTexture(8,1);
                    if(stage==2) { scene.graphTexture(0,2); scene.graphTexture(2,3); }
                    scene.draw(new int[]{113,114,112}[stage],NativeSceneFrame.OIT_BOUNDS+stage,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                }
                renderer.presentPrepared(chain,frame); var capture=awaitCapture(renderer,n); int center=(32*128+64)*4;
                assertEquals(n<3?128:0,capture.rgba()[center]&255,3); assertEquals(128,capture.rgba()[center+1]&255,3);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void eliminatesAdditiveTransmittanceTextureGroupAcrossSlots(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var arena=Arena.ofConfined()) {
            for(int n=1;n<=4;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE|NativeRenderer.IMPROVED_TRANSPARENCY;
                frame.clearColor=new float[]{0,0,0,1};
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(768,256); identity(constants,0); identity(constants,64); identity(constants,512);
                for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
                constants.set(JAVA_FLOAT,272,1000); constants.set(JAVA_FLOAT,276,2000);
                constants.set(JAVA_FLOAT,280,1000); constants.set(JAVA_FLOAT,284,2000);
                var vertex=arena.allocate(64,4); float[][] xy={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
                for(int i=0;i<4;i++) { long at=i*16L; vertex.set(JAVA_FLOAT,at,xy[i][0]); vertex.set(JAVA_FLOAT,at+4,xy[i][1]);
                    vertex.set(JAVA_FLOAT,at+8,.5f); vertex.set(JAVA_INT,at+12,n<3?0x800000ff:0x8000ff00); }
                int cb=scene.copy(constants),vb=scene.borrow(vertex);
                for(int stage=0;stage<3;stage++) {
                    int first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.constant(2,cb,512,256);
                    if(stage>0) scene.graphTexture(8,1);
                    if(stage==2) { scene.graphTexture(0,2); scene.graphTexture(2,3); }
                    scene.draw(new int[]{131,132,130}[stage],NativeSceneFrame.OIT_BOUNDS+stage,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                }
                renderer.presentPrepared(chain,frame); var capture=awaitCapture(renderer,n); int center=(32*128+64)*4;
                assertEquals(n<3?128:0,capture.rgba()[center]&255,3); assertEquals(n<3?0:128,capture.rgba()[center+1]&255,3);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void compositesDynamicOitWithoutTerrainAndReusesGeometryAcrossSlots(NativeEnvironment.Backend backend) throws Exception {
        var messages=new ArrayList<String>();
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(messages));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var arena=Arena.ofConfined()) {
            for(int n=1;n<=4;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE|NativeRenderer.IMPROVED_TRANSPARENCY;
                frame.clearColor=new float[]{0,0,0,1};
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64); identity(constants,256);
                for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
                var vertex=arena.allocate(64,4);
                float[][] xy={{-.8f,-.8f},{-.8f,.8f},{.8f,.8f},{.8f,-.8f}};
                for(int i=0;i<4;i++) { long at=i*16L; vertex.set(JAVA_FLOAT,at,xy[i][0]); vertex.set(JAVA_FLOAT,at+4,xy[i][1]);
                    vertex.set(JAVA_FLOAT,at+8,.5f); vertex.set(JAVA_INT,at+12,0x800000ff); }
                int cb=scene.copy(constants),vb=scene.borrow(vertex);
                var mask=arena.allocate(48,4);
                for(int i=0;i<4;i++) { mask.set(JAVA_FLOAT,i*12L,xy[i][0]); mask.set(JAVA_FLOAT,i*12L+4,xy[i][1]); mask.set(JAVA_FLOAT,i*12L+8,.75f); }
                int maskVertex=scene.borrow(mask),maskBindings=scene.bindingCount();
                scene.constant(0,cb,0,256); scene.constant(1,cb,256,256);
                scene.draw(190,NativeSceneFrame.WATER_MASK,maskVertex,-1,4,0,0,maskBindings,1,-1,0,0,0,0,0,128,64);
                for(int stage=0;stage<3;stage++) {
                    int first=scene.bindingCount(); scene.constant(0,cb,0,256); scene.constant(1,cb,256,256);
                    if(stage>0) scene.graphTexture(8,1);
                    if(stage==2) { scene.graphTexture(0,2); scene.graphTexture(2,3); }
                    scene.draw(new int[]{91,92,90}[stage],NativeSceneFrame.OIT_BOUNDS+stage,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,128,64);
                }
                renderer.presentPrepared(chain,frame); var capture=awaitCapture(renderer,n); int center=(32*128+64)*4;
                assertEquals(128,capture.rgba()[center]&255,3); assertEquals(0,capture.rgba()[center+1]&255);
                assertEquals(0,capture.rgba()[0]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void expandsPixelSizedPointsWithTheVanillaShader(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,128,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var arena=Arena.ofConfined()) {
            var frame=NativeTestSupport.solidFrame(1); frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{0,0,0,1};
            var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
            var constants=arena.allocate(768,256); identity(constants,0); identity(constants,64); identity(constants,512);
            for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
            constants.set(JAVA_FLOAT,288,128); constants.set(JAVA_FLOAT,292,64);
            var vertex=arena.allocate(20,4); vertex.set(JAVA_FLOAT,8,.5f); vertex.set(JAVA_INT,12,0xff0000ff); vertex.set(JAVA_FLOAT,16,12);
            int cb=scene.copy(constants),vb=scene.borrow(vertex),first=scene.bindingCount();
            scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.constant(2,cb,512,256);
            scene.draw(22,NativeSceneFrame.SOLID,vb,-1,1,0,0,first,1,-1,0,0,0,0,0,128,64);
            renderer.presentPrepared(chain,frame); var capture=awaitCapture(renderer,1);
            assertEquals(255,capture.rgba()[(32*128+64)*4]&255);
            assertEquals(0,capture.rgba()[(32*128+72)*4]&255);
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void samplesCubeFacesAndReusesTextureBindings(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,127,65,NativeSwapchain.PresentMode.IMMEDIATE);
            var cube=renderer.createTexture(4,4,1,6,NativeTexture.Format.RGBA8,NativeTexture.CUBE);
            var arena=Arena.ofConfined()) {
            int[] colors={0xff0000ff,0xff00ff00,0xffff0000,0xff00ffff,0xffffff00,0xffff00ff};
            for(int face=0;face<6;face++) {
                var pixels=arena.allocate(64,4); for(int i=0;i<16;i++) pixels.set(JAVA_INT,i*4L,colors[face]);
                cube.uploadLayer(0,face,0,0,4,4,16,pixels);
            }
            // The negative-Z face is visible from the center with an identity view.
            float[] positions={1,-1,-1,1,1,-1,-1,1,-1,-1,-1,-1};
            for(int n=1;n<=4;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE;
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(512,256); identity(constants,0); identity(constants,64);
                for(int i=0;i<4;i++) constants.set(JAVA_FLOAT,128+i*4L,1);
                new org.joml.Matrix4f().setPerspective((float)Math.toRadians(85),127f/65,10,.05f,true).get(constants.asSlice(256,64).asByteBuffer().order(java.nio.ByteOrder.nativeOrder()));
                int cb=scene.copy(constants),vb=scene.copy(MemorySegment.ofArray(positions)),first=scene.bindingCount();
                scene.constant(0,cb,0,256); scene.constant(1,cb,256,256); scene.texture(0,cube.id(),0);
                scene.draw(170,NativeSceneFrame.PANORAMA,vb,-1,4,0,0,first,1,-1,0,0,0,0,0,127,65);
                renderer.presentPrepared(chain,frame); var capture=awaitCapture(renderer,n); int at=(32*127+63)*4;
                // The cube face order is the RHI +X,-X,+Y,-Y,+Z,-Z order.
                assertEquals(255,capture.rgba()[at]&255); assertEquals(0,capture.rgba()[at+1]&255); assertEquals(255,capture.rgba()[at+2]&255);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void drawsConvertedVanillaGeometryWithNonzeroOffsetsAndStableOrder(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,127,65,NativeSwapchain.PresentMode.IMMEDIATE);
            var arena=Arena.ofConfined()) {
            for(int n=1;n<=4;n++) {
                var frame=NativeTestSupport.solidFrame(n); frame.flags=NativeRenderer.CAPTURE; frame.clearColor=new float[]{0,0,0,1};
                var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                var constants=arena.allocate(768,256); identity(constants,256); identity(constants,320);
                for(int c=0;c<4;c++) constants.set(JAVA_FLOAT,384+c*4L,1);
                identity(constants,512); int cb=scene.copy(constants);
                var vertices=arena.allocate(96,8);
                float[][] positions={{-.8f,.2f},{-.8f,.8f},{-.2f,.8f},{-.2f,.2f}};
                for(int i=0;i<4;i++) { long at=32+i*16L; vertices.set(JAVA_FLOAT,at,positions[i][0]); vertices.set(JAVA_FLOAT,at+4,positions[i][1]);
                    vertices.set(JAVA_FLOAT,at+8,.5f); vertices.set(JAVA_INT,at+12,0xff0000ff); }
                int vb=scene.borrow(vertices);
                int first=scene.bindingCount(); scene.constant(0,cb,256,256); scene.constant(1,cb,512,256);
                scene.draw(23,NativeSceneFrame.SOLID,vb,-1,4,0,0,first,1,-1,32,0,0,0,0,127,65);
                renderer.presentPrepared(chain,frame);
                NativeRenderer.Capture capture=null; long deadline=System.nanoTime()+Duration.ofSeconds(5).toNanos();
                while(capture==null && System.nanoTime()<deadline) { capture=renderer.pollCapture(n).orElse(null); if(capture==null) LockSupport.parkNanos(1_000_000); }
                assertNotNull(capture);
                int top=(10*127+20)*4,bottom=(55*127+20)*4;
                assertEquals(255,capture.rgba()[top]&255); assertEquals(0,capture.rgba()[top+1]&255);
                assertEquals(0,capture.rgba()[bottom]&255);
            }
        }
    }
}
