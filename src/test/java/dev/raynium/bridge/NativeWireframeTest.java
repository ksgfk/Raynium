package dev.raynium.bridge;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.locks.LockSupport;
import org.joml.Matrix4f;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeWireframeTest {
    @TempDir Path gameDirectory;
    private static final int[] ALL={0,1,2,2,3,0,4,5,6,6,7,4};

    private static byte[] corner(boolean reverse) {
        float[][][] faces={{{0,-.5f,0},{1,-.5f,0},{1,.5f,0},{0,.5f,0}},
                {{0,-.5f,0},{0,.5f,0},{0,.5f,1},{0,-.5f,1}}};
        var bytes=ByteBuffer.allocate(224).order(ByteOrder.nativeOrder());
        for(int n=0;n<2;n++) {
            int face=reverse?1-n:n;
            for(var p:faces[face]) bytes.putFloat(p[0]).putFloat(p[1]).putFloat(p[2])
                    .putInt(face==0?0xff0000ff:0xff00ff00).putFloat(.5f).putFloat(.5f).putInt(0x00f000f0);
        }
        return bytes.array();
    }

    private static NativeRenderer.Frame frame(long sequence,int epoch,int atlas,int light,long mesh,int mask) {
        var f=NativeTestSupport.solidFrame(sequence); f.epoch=epoch;
        f.flags=NativeRenderer.WIREFRAME;
        f.view=new Matrix4f().translation(0,0,.5f).rotateY(-(float)Math.PI/4).scale(.6f).get(new float[16]);
        f.terrainAlbedoTexture=atlas; f.terrainLightTexture=light;
        f.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
        f.visible=List.of(new NativeRenderer.Visible(mesh,mask,0,0,1));
        return f;
    }

    private static void submit(NativeRenderer renderer,NativeTexture atlas,NativeTexture first,NativeTexture second,NativeRenderer.Frame frame) {
        var light=frame.terrainLightTexture==second.id()?second:first;
        try(var color=renderer.createTexture(128,128,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
            var depth=renderer.createTexture(128,128,1,1,NativeTexture.Format.DEPTH32F,NativeTexture.ATTACHMENT);
            var storage=new NativeFrameBuffer(); var tape=new dev.raynium.client.renderpearl.CommandTape(renderer)) {
            var view=tape.addView(storage,storage.marshal(frame),atlas,light);
            dev.raynium.client.renderpearl.TerrainTestPass.record(tape,view,color,depth,atlas,light);
            tape.captureColor(new dev.raynium.client.renderpearl.CommandTape.Region(color,0,0,0,0,128,128),frame.outputSequence);
            tape.submit();
        }
    }
    private static byte[] await(NativeRenderer renderer,long sequence) {
        long deadline=System.nanoTime()+5_000_000_000L;
        while(System.nanoTime()<deadline) {
            var capture=renderer.pollCapture(sequence);
            if(capture.isPresent()) return capture.get().rgba();
            LockSupport.parkNanos(100_000);
        }
        throw new AssertionError("Wireframe capture timed out");
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void groupedGeometryMatchesOriginalOrderAcrossMasksSlotsEpochAndSubmissionModes(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime()) {
            for(var submission:List.of(TerrainSubmission.DIRECT,TerrainSubmission.INDIRECT)) {
                try(var renderer=runtime.createRenderer(NativeTestSupport.shaders(),submission);
                    var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE);
                    var atlas=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
                    var light=renderer.createTexture(16,16,1,NativeTexture.Format.RGBA8);
                    var otherLight=renderer.createTexture(16,16,1,NativeTexture.Format.RGBA8)) {
                    org.junit.jupiter.api.Assumptions.assumeTrue(runtime.deviceInfo().wireframe());
                    atlas.upload(0,0,0,1,1,new byte[]{-1,-1,-1,-1});
                    byte[] lighting=new byte[1024]; Arrays.fill(lighting,(byte)255);
                    light.upload(0,0,0,16,16,lighting); otherLight.upload(0,0,0,16,16,lighting);
                    long sequence=0; byte[] earlierReference=null;
                    for(int epoch=1;epoch<=2;epoch++) {
                        byte[] vertices=corner(epoch==2);
                        var reference=frame(++sequence,epoch,atlas.id(),light.id(),2,-1);
                        reference.meshes=List.of(new NativeRenderer.Mesh(1,vertices,8,0),new NativeRenderer.Mesh(2,vertices,8,2),new NativeRenderer.Mesh(3,vertices,8,1));
                        reference.indices=List.of(new NativeRenderer.Index(2,ALL));
                        submit(renderer,atlas,light,otherLight,reference); byte[] expected=await(renderer,sequence);
                        int colored=0; for(int i=0;i<expected.length;i+=4) if(expected[i]!=0 || expected[i+1]!=0) colored++;
                        assertTrue(colored>100,"Reference corner must be visible");
                        if(earlierReference!=null) assertFalse(Arrays.equals(earlierReference,expected),"The reference must expose shared-edge ordering");
                        earlierReference=expected;
                        long solid=++sequence; submit(renderer,atlas,light,otherLight,frame(solid,epoch,atlas.id(),light.id(),1,-1));
                        long cutout=++sequence; submit(renderer,atlas,light,otherLight,frame(cutout,epoch,atlas.id(),light.id(),3,-1));
                        var off=frame(++sequence,epoch,atlas.id(),light.id(),1,-1); off.flags=0;
                        submit(renderer,atlas,light,otherLight,off);
                        assertArrayEquals(expected,await(renderer,solid),"Solid wireframe input order");
                        assertArrayEquals(expected,await(renderer,cutout),"Cutout wireframe input order");
                        var grown=frame(++sequence,epoch,atlas.id(),otherLight.id(),1,-1);
                        grown.meshes=List.of(new NativeRenderer.Mesh(99,new byte[70000*28],70000,0));
                        submit(renderer,atlas,light,otherLight,grown); assertArrayEquals(expected,await(renderer,sequence),"Reenabled after shared-index growth and material rebind");
                        for(int mask=1;mask<=2;mask++) {
                            int face=mask==1?1:0,base=(epoch==2?1-face:face)*4;
                            int[] indices={base,base+1,base+2,base+2,base+3,base,base,base+1,base+2,base+2,base+3,base};
                            var selectedReference=frame(++sequence,epoch,atlas.id(),otherLight.id(),2,-1);
                            selectedReference.indices=List.of(new NativeRenderer.Index(2,indices));
                            submit(renderer,atlas,light,otherLight,selectedReference); byte[] selected=await(renderer,sequence);
                            submit(renderer,atlas,light,otherLight,frame(++sequence,epoch,atlas.id(),otherLight.id(),1,mask));
                            assertArrayEquals(selected,await(renderer,sequence),"Explicit face mask "+mask);
                        }
                        var removed=NativeTestSupport.solidFrame(++sequence); removed.epoch=epoch;
                        removed.removedMeshes=new long[]{1,2,3,99}; submit(renderer,atlas,light,otherLight,removed);
                    }
                }
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void originalOrderSurvivesBothGeometryPoolsCrossingPagesAndUnsubmittedCacheClose(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE);
            var atlas=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
            var light=renderer.createTexture(16,16,1,NativeTexture.Format.RGBA8)) {
            org.junit.jupiter.api.Assumptions.assumeTrue(runtime.deviceInfo().wireframe());
            atlas.upload(0,0,0,1,1,new byte[]{-1,-1,-1,-1});
            byte[] lighting=new byte[1024]; Arrays.fill(lighting,(byte)255); light.upload(0,0,0,16,16,lighting);
            var first=frame(1,1,atlas.id(),light.id(),2,-1);
            first.meshes=List.of(new NativeRenderer.Mesh(2,corner(false),8,2));
            first.indices=List.of(new NativeRenderer.Index(2,ALL));
            submit(renderer,atlas,light,light,first); byte[] expected=await(renderer,1);

            // Six live 28 MiB vertex and 6 MiB index intervals exceed both 32 MiB pool pages.
            byte[] filler=new byte[(1<<20)*28]; int[] fillerIndices=new int[(1<<20)/4*6];
            var meshes=new ArrayList<NativeRenderer.Mesh>(); var indices=new ArrayList<NativeRenderer.Index>();
            for(int i=0;i<6;i++) {
                meshes.add(new NativeRenderer.Mesh(100+i,filler,1<<20,2));
                indices.add(new NativeRenderer.Index(100+i,fillerIndices));
            }
            meshes.add(new NativeRenderer.Mesh(1,corner(false),8,0));
            var grown=frame(2,1,atlas.id(),light.id(),1,-1); grown.meshes=meshes; grown.indices=indices;
            submit(renderer,atlas,light,light,grown); assertArrayEquals(expected,await(renderer,2),"Both pools crossed pages before the visible wireframe allocation");

            var retired=frame(3,1,atlas.id(),light.id(),1,-1); retired.flags=0;
            retired.removedMeshes=new long[]{100,101,102,103,104,105}; submit(renderer,atlas,light,light,retired);
            var failed=frame(4,1,atlas.id(),light.id(),1,-1);
            failed.instances=List.of();
            assertThrows(IllegalStateException.class,()->submit(renderer,atlas,light,light,failed));
        }
    }
}
