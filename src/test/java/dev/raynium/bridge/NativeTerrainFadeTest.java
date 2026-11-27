package dev.raynium.bridge;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.TerrainTestPass;
import java.nio.file.Path;
import java.util.*;
import org.joml.Matrix4f;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeTerrainFadeTest {
    @TempDir Path directory;
    private static final int SIZE=8, START=0x7fff_ff80;
    private static final int[] INDICES={0,1,2,2,3,0,4,5,6,6,7,4};

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void sectionFadeMatchesCpuColorsAcrossTerrainStages(NativeEnvironment.Backend backend) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var atlas=renderer.createTexture(1,1,1,1,NativeTexture.Format.RGBA8);
            var light=renderer.createTexture(1,1,1,1,NativeTexture.Format.RGBA8)) {
            atlas.upload(0,0,0,1,1,new byte[]{-1,-1,-1,(byte)192});
            light.upload(0,0,0,1,1,new byte[]{-1,-1,-1,-1});
            for(int stage=0;stage<4;stage++) {
                byte[] full=pixels(renderer,atlas,light,stage,40,200,START,START,0);
                byte[] half=pixels(renderer,atlas,light,stage,40,100,0,START,0);
                byte[] dark=pixels(renderer,atlas,light,stage,40,0,0,START,0);
                String label=backend+" stage "+stage;
                assertFalse(Arrays.equals(full,half),label+" requires visible colored geometry");
                assertFalse(Arrays.equals(dark,half),label+" requires a distinguishable midpoint");
                assertArrayEquals(half,pixels(renderer,atlas,light,stage,40,200,START,START+375,750),label+" midpoint");
                assertArrayEquals(dark,pixels(renderer,atlas,light,stage,40,200,START,START-1,750),label+" future upload");
                assertArrayEquals(full,pixels(renderer,atlas,light,stage,40,200,START,START+750,750),label+" complete");
                assertArrayEquals(full,pixels(renderer,atlas,light,stage,40,200,START,0,750),label+" clock horizon");
                assertArrayEquals(full,pixels(renderer,atlas,light,stage,40,200,0,START,750),label+" zero start");
                assertArrayEquals(full,pixels(renderer,atlas,light,stage,0,200,START,START,750),label+" near exemption");
            }
        }
    }

    private static byte[] pixels(NativeRenderer renderer,NativeTexture atlas,NativeTexture light,int stage,int origin,
                                 int red,int start,int now,int duration) {
        int layer=Math.min(stage,2);
        var frame=NativeTestSupport.solidFrame(NativeRenderer.nextFrameId());
        frame.epoch=frame.outputSequence;
        frame.fadeNowMs=now; frame.fadeDurationMs=duration;
        frame.fogColor=new float[]{0,0,0,1};
        frame.view=new Matrix4f().translation(-origin,0,0).get(new float[16]);
        frame.flags=stage==3?NativeRenderer.IMPROVED_TRANSPARENCY:0;
        frame.terrainAlbedoTexture=atlas.handle(); frame.terrainLightTexture=light.handle();
        frame.meshes=List.of(new TerrainFixture.Mesh(1,NativeRenderPearlTest.terrainQuad(red),8,layer));
        if(layer==2) frame.indices=List.of(new TerrainFixture.Index(1,INDICES));
        frame.instances=List.of(new TerrainFixture.Instance(origin,0,0,start));
        frame.visible=List.of(new TerrainFixture.Visible(1,-1,0,0,1));
        int flags=NativeTexture.ATTACHMENT|NativeTexture.READBACK;
        try(var storage=new NativeFrameBuffer(); var tape=new CommandTape(renderer);
            var depth=renderer.createTexture(SIZE,SIZE,1,1,NativeTexture.Format.DEPTH32F,NativeTexture.ATTACHMENT);
            var color=renderer.createTexture(SIZE,SIZE,1,1,stage==3?NativeTexture.Format.RGBA16F:NativeTexture.Format.RGBA8,flags);
            var bounds=stage==3?renderer.createTexture(SIZE,SIZE,1,1,NativeTexture.Format.RGBA32F,flags):null;
            var first=stage==3?renderer.createTexture(SIZE,SIZE,1,1,NativeTexture.Format.RGBA16F,flags):null;
            var second=stage==3?renderer.createTexture(SIZE,SIZE,1,1,NativeTexture.Format.RGBA16F,flags):null) {
            var view=tape.addView(storage,frame.marshal(storage),atlas,light);
            if(stage==3) TerrainTestPass.recordOit(tape,view,depth,atlas,light,bounds,first,second,color);
            else TerrainTestPass.record(tape,view,color,depth,atlas,light);
            var targets=stage==3?List.of(bounds,first,second,color):List.of(color);
            int bytes=SIZE*SIZE*(stage==3?40:4);
            try(var read=renderer.createBuffer(GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,bytes,null,()->tape);
                var mapped=read.map(true,false); var fence=tape.createFence()) {
                int offset=0;
                for(var target:targets) {
                    tape.textureToBuffer(new CommandTape.Region(target,0,0,0,0,SIZE,SIZE),read,offset,null);
                    offset+=SIZE*SIZE*target.format().bytesPerPixel();
                }
                tape.submit();
                assertTrue(fence.awaitCompletion(5_000_000_000L));
                byte[] result=new byte[bytes];
                mapped.data().get(0,result);
                return result;
            }
        }
    }
}
