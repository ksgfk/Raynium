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
    static List<NativeSharedTextures.Texture> textures(boolean split) {
        return List.of(new NativeSharedTextures.Texture(17,9,5,split),new NativeSharedTextures.Texture(16,16,1,false));
    }
    static NativeRenderer.Mesh mesh(long id,byte[] vertices) {
        return new NativeRenderer.Mesh(id,vertices,1,vertices.length/28,28,NativeRenderer.POSITION_COLOR_UV_LIGHT,List.of(new NativeRenderer.Submesh(0,6,0)));
    }
    static NativeRenderer.Material material(int anisotropy) { return new NativeRenderer.Material(1,NativeRenderer.TEXTURED,0,1,anisotropy); }
    static NativeRenderer.Frame frame(long epoch,long input,long output,boolean direct) {
        var f=new NativeRenderer.Frame(); f.epoch=epoch; f.inputSequence=input; f.outputSequence=output;
        f.flags=(direct?NativeRenderer.DIRECT:0)|(Boolean.getBoolean("raynium.terrain.profile")?NativeRenderer.PROFILE:0);
        f.changedTextures=3; f.fogColor=new float[]{0,0,0,1}; return f;
    }
    @ParameterizedTest @MethodSource("paths")
    void mixedProgramsShareOneVisibleList(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer();
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                byte[] center=quad(0); var vertices=ByteBuffer.wrap(center).order(ByteOrder.nativeOrder());
                for(int v=0;v<4;v++) { vertices.putFloat(v*28,vertices.getFloat(v*28)*.5f); vertices.putFloat(v*28+4,vertices.getFloat(v*28+4)*.5f); }
                for(int sequence=1;sequence<=4;sequence++) {
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true);
                    var packet=frame(1,sequence,sequence,sequence%2==0);
                    if(sequence==1) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.meshes=List.of(mesh(1,center),new NativeRenderer.Mesh(2,new byte[0],0,3,0,NativeRenderer.PROCEDURAL,List.of(new NativeRenderer.Submesh(0,3,0))));
                        packet.materials=List.of(material(1),new NativeRenderer.Material(2,NativeRenderer.UV,0,0,1));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1),new NativeRenderer.Primitive(2,2,2));
                    }
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.visible=List.of(new NativeRenderer.Visible(1,0,0,0,1),new NativeRenderer.Visible(2,0,0,0,1));
                    renderer.render(target,packet);
                    int[] color={32,64,128};
                    producer.copyAndCheckImage(output,sequence,32,32,(x,y)->x>=8 && x<24 && y>=8 && y<24?color:null);
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void samplesAllMipsThroughBothSharingPaths(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer();
            var terrain=renderer.createSharedTextures(textures(split));
            var preview=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,preview.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(int i=0;i<terrain.images().size();i++) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,terrain.images().get(i),!split && i==0?5:1,true));
                assertThrows(IllegalStateException.class,runtime::close);
                for(int frame=0;frame<10;frame++) {
                    int mip=frame%5; long sequence=frame+1;
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence);
                    var packet=frame(1,sequence,sequence,frame<5);
                    packet.indices=frame==0?List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0})):List.of();
                    packet.materials=frame==0?List.of(material(1)):List.of();
                    packet.meshes=List.of(mesh(sequence,quad(mip)));
                    packet.primitives=List.of(new NativeRenderer.Primitive(sequence,sequence,1));
                    packet.visible=List.of(new NativeRenderer.Visible(sequence,0,0,0,1));
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.removedMeshes=packet.removedPrimitives=frame==0?new long[0]:new long[]{sequence-1};
                    renderer.render(preview,packet);
                    producer.copyAndCheckSolid(output,sequence,32,32,new int[]{32+mip*32,64+mip*16,128});
                }
                producer.writeTerrainInputs(inputs,split,17,9,5,11);
                renderer.render(preview,frame(1,11,11,false));
                producer.copyAndCheckSolid(output,11,32,32,new int[]{0,0,0});
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void reversedDepthEpochAndLargeCoordinates(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer();
            var terrain=renderer.createSharedTextures(textures(split));
            var preview=renderer.createTarget(32,32);
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
                for(int number=1;number<=6;number++) {
                    producer.writeTerrainInputs(inputs,split,17,9,5,number,true);
                    var packet=frame(number<3?1:2,number,number,number%2==0);
                    packet.cameraBlock=camera; packet.flags|=number%2==0?NativeRenderer.RGSS:0;
                    if(number==1 || number==4) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.materials=List.of(material(number==1?4:1));
                        packet.meshes=List.of(mesh(1,near),mesh(2,far));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1),new NativeRenderer.Primitive(2,2,1));
                    }
                    if(number!=3) {
                        packet.visible=List.of(new NativeRenderer.Visible(1,0,0,0,1),new NativeRenderer.Visible(2,0,0,1,1));
                        packet.instances=List.of(new NativeRenderer.Instance(camera[0],camera[1],camera[2],1),new NativeRenderer.Instance(camera[0],camera[1],camera[2],1));
                    }
                    renderer.render(preview,packet);
                    producer.copyAndCheckSolid(output,number,32,32,number==3?new int[]{0,0,0}:new int[]{32,0,0});
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void sceneSurvivesOutputAndInputRecreation(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer()) {
            for(int generation=0;generation<3;generation++) {
                boolean split=generation%2==1;
                try(var textures=renderer.createSharedTextures(textures(split));
                    var target=renderer.createTarget(32+generation*4,32+generation*4);
                    var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
                    var imports=new ArrayList<VulkanPreviewImage>();
                    try {
                        for(var e:textures.exports()) imports.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                        var packet=frame(1,1,1,generation==1);
                        if(generation==0) {
                            packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                            packet.meshes=List.of(mesh(1,quad(0))); packet.materials=List.of(material(1));
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                        }
                        packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                        packet.visible=List.of(new NativeRenderer.Visible(99,0,0,0,1));
                        // No input signal exists yet. Validation must reject without submitting a GPU wait.
                        assertThrows(IllegalStateException.class,()->renderer.render(target,packet));
                        packet.visible=List.of(new NativeRenderer.Visible(1,0,0,0,1));
                        producer.writeTerrainInputs(imports,split,17,9,5,1);
                        renderer.render(target,packet);
                        producer.copyAndCheckSolid(output,1,32+generation*4,32+generation*4,new int[]{32,64,128});
                    } finally { for(var image:imports) image.close(); }
                }
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void incompatibleDirectImportCanRecoverWithFallback(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer()) {
            int[] fallbacks={0};
            try(var terrain=dev.raynium.client.terrain.TextureTransfer.create("auto",()->{
                try(var direct=renderer.createSharedTextures(textures(false))) {
                    var original=direct.images().getFirst(); byte[] differentGpu=original.luid(); differentGpu[0]^=1;
                    var invalid=new SharedImageInfo(original.handleType(),17,9,original.memoryHandle(),original.readyHandle(),original.consumedHandle(),
                        original.allocationSize(),original.memoryTypeIndex(),original.nodeMask(),differentGpu,original.deviceUuid(),original.driverUuid(),
                        original.nativeFormat(),original.nativeUsage(),original.nativeFlags(),original.nativeTiling(),0,true);
                    try(var unexpected=VulkanPreviewImage.create(producer.device,producer.family,invalid,5,true)) { fail("Mismatched GPU imported"); }
                    return direct;
                }
            },()->renderer.createSharedTextures(textures(true)),failure->fallbacks[0]++)) {
                assertEquals(1,fallbacks[0]); assertEquals(6,terrain.exports().size());
                var inputs=new ArrayList<VulkanPreviewImage>();
                try {
                    for(var info:terrain.images()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,info,1,true));
                    producer.writeTerrainInputs(inputs,true,17,9,5,1);
                } finally { for(var input:inputs) input.close(); }
            }
        }
    }

}
