package dev.raynium.bridge;

import dev.raynium.client.preview.VulkanPreviewImage;
import java.nio.*;
import java.nio.file.Path;
import java.util.*;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.MethodSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeTransparencyTest {
    @TempDir Path gameDirectory;
    static java.util.stream.Stream<org.junit.jupiter.params.provider.Arguments> paths() { return NativeTerrainTest.paths(); }
    private static byte[] quads(float[] distances,int[] colors) {
        var data=ByteBuffer.allocate(distances.length*112).order(ByteOrder.nativeOrder());
        for(int q=0;q<distances.length;q++) {
            float d=distances[q];
            for(float[] xy:new float[][]{{-1,-1},{1,-1},{1,1},{-1,1}})
                data.putFloat(xy[0]*d).putFloat(xy[1]*d).putFloat(-d).putInt(colors[q])
                    .putFloat(.5f).putFloat(.5f).putShort((short)240).putShort((short)240);
        }
        return data.array();
    }
    private static int[] indices(int quads,boolean reverse) {
        int[] values=new int[quads*6];
        for(int i=0;i<quads;i++) {
            int v=(reverse?quads-i-1:i)*4,o=i*6;
            values[o]=v; values[o+1]=v+1; values[o+2]=v+2; values[o+3]=v+2; values[o+4]=v+3; values[o+5]=v;
        }
        return values;
    }
    @ParameterizedTest @MethodSource("paths")
    void cutoutSortedIndicesWaveletsAndModeSwitches(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(NativeTerrainTest.textures(split))) {
            var inputs=new ArrayList<VulkanPreviewImage>(); long sequence=0;
            byte[] previousGeometry=null; int[] previousOrder=null; int previousMode=-1;
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                for(int size:new int[]{32,40,32}) try(var target=renderer.createTarget(size,size);
                    var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
                    for(int scenario=0;scenario<12;scenario++) {
                        long seq=++sequence; var f=NativeTerrainTest.frame(1,seq,scenario+1);
                        f.projection=new float[]{1,0,0,0,0,1,0,0,0,0,0,-1,0,0,.1f,0};
                        f.changedTextures=seq==1?3:0;
                        boolean oit=scenario==3 || scenario==4 || scenario>=7;
                        f.flags=(oit?NativeRenderer.IMPROVED_TRANSPARENCY:0)|(Boolean.getBoolean("raynium.terrain.profile")?NativeRenderer.PROFILE:0);
                        int mode=scenario<=1?1:2;
                        float[] distances={3,1}; int[] colors={0x80ff0000,0x800000ff};
                        int[] expected={16,0,32,192};
                        if(scenario<=1) { distances=new float[]{1}; colors=new int[]{scenario==0?0x7fffffff:0x80ffffff}; expected=scenario==0?new int[]{0,0,0,0}:new int[]{32,64,128,128}; }
                        if(scenario==5) expected=new int[]{16,0,0,128};
                        if(scenario==7) { colors[1]=0xff0000ff; expected=new int[]{32,0,0,255}; }
                        if(scenario==8) { distances=new float[20]; colors=new int[20]; Arrays.fill(colors,0x80ffffff); for(int i=0;i<20;i++) distances[i]=1+i; expected=new int[]{32,64,128,255}; }
                        if(scenario==9) { distances=new float[]{1}; colors=new int[]{0x17ffffff}; expected=new int[]{0,0,0,0}; }
                        if(scenario==10) expected=new int[]{0,0,0,0};
                        // A narrow range with a single color still has the analytically known coverage.
                        if(scenario==11) { distances=new float[]{1.00001f,1}; colors=new int[]{0x80ffffff,0x80ffffff}; expected=new int[]{24,48,96,192}; }
                        int[] order=indices(distances.length,scenario==4 || scenario==5);
                        byte[] geometry=quads(distances,colors);
                        if(!Arrays.equals(previousOrder,order)) f.indices=List.of(new NativeRenderer.Index(1,order));
                        if(!Arrays.equals(previousGeometry,geometry)) f.meshes=List.of(new NativeRenderer.Mesh(1,geometry,1,geometry.length/28,List.of(new NativeRenderer.Submesh(0,order.length,0))));
                        if(mode!=previousMode) f.materials=List.of(new NativeRenderer.Material(1,0,1,1,mode));
                        if(seq==1) f.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                        previousGeometry=geometry; previousOrder=order; previousMode=mode;
                        if(scenario!=10) {
                            f.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1));
                            f.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                        }
                        producer.writeTerrainInputs(inputs,split,17,9,5,seq,false,f.changedTextures);
                        renderer.render(target,f);
                        producer.copyAndCheckSolid(output,scenario+1,size,size,expected);
                    }
                }
            } finally { for(var input:inputs) input.close(); }
        }
    }
}
