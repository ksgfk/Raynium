package dev.raynium.bridge;

import dev.raynium.client.preview.VulkanPreviewImage;
import java.nio.*;
import java.nio.file.Path;
import java.util.*;
import java.util.stream.Stream;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.*;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeTerrainTest {
    @TempDir Path gameDirectory;
    static Stream<Arguments> paths() {
        return Arrays.stream(NativeEnvironment.Backend.values()).flatMap(b->Stream.of(Arguments.of(b,false),Arguments.of(b,true)));
    }
    static float[] identity() { return new float[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
    static byte[] quad(int mip) {
        var vertices=ByteBuffer.allocate(112).order(ByteOrder.nativeOrder());
        float u=(32f/17)*(1<<mip),v=(32f/9)*(1<<mip);
        for(float[] p:new float[][]{{-1,-1,0,0},{1,-1,u,0},{1,1,u,v},{-1,1,0,v}})
            vertices.putFloat(p[0]).putFloat(p[1]).putFloat(.5f).putInt(-1).putFloat(p[2]).putFloat(p[3]).putShort((short)240).putShort((short)240);
        return vertices.array();
    }
    @ParameterizedTest @MethodSource("paths")
    void samplesAllMipsThroughBothSharingPaths(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var terrain=runtime.createTerrain(17,9,5,split);
            var preview=runtime.createPreview(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,preview.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(int i=0;i<terrain.images().size();i++) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,terrain.images().get(i),!split && i==0?5:1,true));
                assertThrows(IllegalStateException.class,runtime::close);
                preview.draw(1);
                producer.copyAndCheck(output,1,32,32);
                for(int frame=0;frame<10;frame++) {
                    int mip=frame%5; long sequence=frame+1;
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence);
                    var packet=new NativeTerrain.Frame(1,sequence,frame>=5,true,false,1,List.of(new NativeTerrain.Mesh(sequence,quad(mip))),
                        List.of(new NativeTerrain.Draw(sequence,0,0,0,1)),frame==0?new long[0]:new long[]{sequence-1},
                        identity(),identity(),new int[3],new float[3],new float[]{0,0,0,1},new float[]{100,200,100,200});
                    terrain.draw(preview,sequence+1,packet);
                    producer.copyAndCheckSolid(output,sequence+1,32,32,new int[]{32+mip*32,64+mip*16,128});
                }
                producer.writeTerrainInputs(inputs,split,17,9,5,11);
                terrain.draw(preview,12,new NativeTerrain.Frame(1,11,false,true,false,1,List.of(),
                    List.of(new NativeTerrain.Draw(10,0,0,0,1,0)),new long[0],identity(),identity(),new int[3],new float[3],
                    new float[]{0,0,0,1},new float[]{100,200,100,200}));
                producer.copyAndCheckSolid(output,12,32,32,new int[]{0,0,0});
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void reversedDepthEpochAndLargeCoordinates(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var terrain=runtime.createTerrain(17,9,5,split);
            var preview=runtime.createPreview(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,preview.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(int i=0;i<terrain.images().size();i++) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,terrain.images().get(i),!split && i==0?5:1,true));
                byte[] near=quad(0),far=quad(0);
                for(int v=0;v<4;v++) {
                    ByteBuffer.wrap(near).order(ByteOrder.nativeOrder()).putFloat(v*28+8,.8f).putInt(v*28+12,0xff0000ff);
                    ByteBuffer.wrap(far).order(ByteOrder.nativeOrder()).putFloat(v*28+8,.2f).putInt(v*28+12,0xffff0000);
                }
                int[] camera={30000000,-64,-30000000};
                var draws=List.of(new NativeTerrain.Draw(1,camera[0],camera[1],camera[2],1),new NativeTerrain.Draw(2,camera[0],camera[1],camera[2],1));
                for(int frame=1;frame<=6;frame++) {
                    producer.writeTerrainInputs(inputs,split,17,9,5,frame,true);
                    var updates=frame==1?List.of(new NativeTerrain.Mesh(1,near),new NativeTerrain.Mesh(2,far)):List.<NativeTerrain.Mesh>of();
                    var packet=new NativeTerrain.Frame(frame<3?1:2,frame,frame%2==0,true,frame%2==0,frame<3?4:1,updates,draws,new long[0],
                        identity(),identity(),camera,new float[3],new float[]{0,0,0,1},new float[]{100,200,100,200});
                    terrain.draw(preview,frame,packet);
                    producer.copyAndCheckSolid(output,frame,32,32,frame<3?new int[]{32,0,0}:new int[]{0,0,0});
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void incompatibleDirectImportCanRecoverWithFallback(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()))) {
            int[] fallbacks={0};
            try(var terrain=dev.raynium.client.terrain.TextureTransfer.create("auto",()->{
                try(var direct=runtime.createTerrain(17,9,5,false)) {
                    var original=direct.images().getFirst(); byte[] differentGpu=original.luid(); differentGpu[0]^=1;
                    var invalid=new NativePreview.Info(original.handleType(),17,9,original.memoryHandle(),original.readyHandle(),original.consumedHandle(),
                        original.allocationSize(),original.memoryTypeIndex(),original.nodeMask(),differentGpu,original.deviceUuid(),original.driverUuid(),
                        original.nativeFormat(),original.nativeUsage(),original.nativeFlags(),original.nativeTiling(),0,true);
                    try(var unexpected=VulkanPreviewImage.create(producer.device,producer.family,invalid,5,true)) { fail("Mismatched GPU imported"); }
                    return direct;
                }
            },()->runtime.createTerrain(17,9,5,true),failure->fallbacks[0]++)) {
                assertEquals(1,fallbacks[0]); assertTrue(terrain.splitMips());
                var inputs=new ArrayList<VulkanPreviewImage>();
                try {
                    for(var info:terrain.images()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,info,1,true));
                    producer.writeTerrainInputs(inputs,true,17,9,5,1);
                } finally { for(var input:inputs) input.close(); }
            }
        }
    }

}
