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
        return new NativeRenderer.Mesh(id,vertices,1,vertices.length/28,List.of(new NativeRenderer.Submesh(0,6,0)));
    }
    static NativeRenderer.Material material(int anisotropy) { return new NativeRenderer.Material(1,0,1,anisotropy); }
    // Quad occupying the first four vertices of a larger mesh; the rest pads the allocation size.
    static byte[] span(int vertexCount,float offset,float scale) {
        byte[] data=new byte[vertexCount*28];
        byte[] quad=quad(0);
        System.arraycopy(quad,0,data,0,quad.length);
        var vertices=ByteBuffer.wrap(data).order(ByteOrder.nativeOrder());
        for(int v=0;v<4;++v) {
            vertices.putFloat(v*28,vertices.getFloat(v*28)*scale+offset);
            vertices.putFloat(v*28+4,vertices.getFloat(v*28+4)*scale);
        }
        return data;
    }
    static int[] atlas() { return new int[]{32,64,128}; }
    static int[] clear() { return new int[]{0,0,0}; }
    static NativeRenderer.Frame frame(long epoch,long input,long output) {
        var f=new NativeRenderer.Frame(); f.epoch=epoch; f.inputSequence=input; f.outputSequence=output;
        f.flags=(Boolean.getBoolean("raynium.terrain.profile")?NativeRenderer.PROFILE:0);
        f.changedTextures=3; f.fogColor=new float[]{0,0,0,1}; return f;
    }
    @ParameterizedTest @MethodSource("paths")
    void uploadsWithoutDrawsSurviveSlotReuseAndTargetResize(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split))) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                long inputSequence=0;
                for(int size:new int[]{32,36,40,32}) {
                    try(var target=renderer.createTarget(size,size);
                        var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
                        for(int outputSequence=1;outputSequence<=4;++outputSequence) {
                            ++inputSequence;
                            boolean upload=inputSequence==1;
                            var packet=frame(1,inputSequence,outputSequence);
                            packet.changedTextures=upload?3:0;
                            producer.writeTerrainInputs(inputs,split,17,9,5,inputSequence,false,packet.changedTextures);
                            if(upload) {
                                packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                                packet.meshes=List.of(mesh(1,quad(0)));
                                packet.materials=List.of(material(1));
                                packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                            }
                            // Both slots see empty frames before cached geometry and textures are first sampled.
                            if(outputSequence>=3) {
                                packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1));
                                packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                            }
                            renderer.render(target,packet);
                            producer.copyAndCheckSolid(output,outputSequence,size,size,outputSequence>=3?atlas():clear());
                        }
                    }
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void twoPendingSlotsKeepParametersAndReplacedGeometry(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                for(int pair=0;pair<8;++pair) {
                    for(int slot=0;slot<2;++slot) {
                        int sequence=pair*2+slot+1;
                        var packet=frame(1,sequence,sequence);
                        packet.changedTextures=slot==0?3:0;
                        byte[] data=quad(0); var vertices=ByteBuffer.wrap(data).order(ByteOrder.nativeOrder());
                        for(int v=0;v<4;++v) {
                            vertices.putFloat(v*28,vertices.getFloat(v*28)*.25f);
                            vertices.putFloat(v*28+4,vertices.getFloat(v*28+4)*.25f);
                            vertices.putInt(v*28+12,slot==0?0xff0000ff:0xff00ff00);
                        }
                        packet.meshes=List.of(mesh(1,data));
                        if(sequence==1) {
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                        }
                        packet.materials=List.of(material(slot==0?4:1));
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.cameraBlock=new int[]{30000000,-64,-30000000};
                        packet.cameraFraction=new float[]{slot*.25f,0,0};
                        packet.fogColor=new float[]{sequence/255f,0,0,1};
                        packet.instances=List.of(new NativeRenderer.Instance(30000000,-64,-30000000,1),new NativeRenderer.Instance(30000001,-64,-30000000,1));
                        packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,2));
                        // Neither input signal exists yet; both native slots must remain in flight.
                        renderer.render(target,packet);
                    }
                    for(int slot=0;slot<2;++slot) {
                        int sequence=pair*2+slot+1,shift=slot*4;
                        int[] color=slot==0?new int[]{32,0,0}:new int[]{0,64,0};
                        producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true,slot==0?3:0);
                        producer.copyAndCheckImage(output,sequence,32,32,(x,y)->y>=12 && y<20 &&
                            ((x>=12-shift && x<20-shift) || (x>=28-shift && x<36-shift)) ? color : new int[]{sequence,0,0});
                    }
                }
                producer.writeTerrainInputs(inputs,split,17,9,5,17,true);
                var empty=frame(1,17,17);
                empty.removedMeshes=empty.removedPrimitives=empty.removedMaterials=empty.removedIndices=new long[]{1};
                renderer.render(target,empty); producer.copyAndCheckSolid(output,17,32,32,new int[]{0,0,0});
                producer.writeTerrainInputs(inputs,split,17,9,5,18,true);
                renderer.render(target,frame(2,18,18)); producer.copyAndCheckSolid(output,18,32,32,new int[]{0,0,0});
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void failedPreparationRetainsResourcesForPendingFrame(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                var initial=frame(1,1,1); initial.changedTextures=3;
                initial.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                initial.meshes=List.of(mesh(1,quad(0))); initial.materials=List.of(material(1));
                initial.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                initial.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                initial.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1));
                producer.writeTerrainInputs(inputs,split,17,9,5,1,true,3);
                renderer.render(target,initial);
                producer.copyAndCheckSolid(output,1,32,32,new int[]{32,64,128});

                var pending=frame(1,2,2); pending.changedTextures=0;
                pending.instances=initial.instances; pending.visible=initial.visible;
                renderer.render(target,pending);

                // Retire the pending frame's resources, then fail before Extract can receive them.
                var failed=frame(1,3,3); failed.changedTextures=0;
                failed.removedPrimitives=failed.removedMeshes=failed.removedIndices=failed.removedMaterials=new long[]{1};
                failed.materials=List.of(new NativeRenderer.Material(0,0,1,1));
                assertThrows(IllegalStateException.class,()->renderer.render(target,failed));

                producer.writeTerrainInputs(inputs,split,17,9,5,2,true,0);
                producer.copyAndCheckSolid(output,2,32,32,new int[]{32,64,128});
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void cachedSubmeshesFollowMaterialIndexAndMeshUpdates(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                var ranges=List.of(new NativeRenderer.Submesh(0,6,0),new NativeRenderer.Submesh(6,6,0));
                for(int pair=0;pair<6;pair++) {
                    for(int slot=0;slot<2;slot++) {
                        int sequence=pair*2+slot+1;
                        var packet=frame(sequence==12?3:sequence==11?2:1,sequence,sequence);
                        packet.changedTextures=sequence==1?3:0;
                        if(sequence==1 || sequence==11) {
                            packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0,4,5,6,6,7,4}));
                            packet.meshes=List.of(new NativeRenderer.Mesh(1,twoQuads(1),1,8,ranges));
                            packet.materials=List.of(material(1),new NativeRenderer.Material(2,1,1,1));
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1),new NativeRenderer.Primitive(2,1,2));
                        }
                        if(sequence==3) packet.materials=List.of(new NativeRenderer.Material(1,0,0,1));
                        if(sequence==5) packet.indices=List.of(new NativeRenderer.Index(1,new int[]{4,5,6,6,7,4,0,1,2,2,3,0,0,1,2,2,3,0}));
                        if(sequence==7) packet.meshes=List.of(new NativeRenderer.Mesh(1,twoQuads(.5f),1,8,ranges));
                        if(sequence==8) packet.materials=List.of(new NativeRenderer.Material(2,0,1,4));
                        if(sequence==9) { packet.removedPrimitives=new long[]{1}; packet.removedMaterials=new long[]{1}; }
                        if(sequence==10) {
                            packet.removedPrimitives=packet.removedMaterials=new long[]{2};
                            packet.removedMeshes=packet.removedIndices=new long[]{1};
                        }
                        if(sequence!=10 && sequence!=12) {
                            packet.instances=List.of(new NativeRenderer.Instance(100,100,0,1),new NativeRenderer.Instance(0,0,0,1));
                            var right=new NativeRenderer.Visible(2,2,8,1,1);
                            packet.visible=sequence==9?List.of(right):List.of(right,new NativeRenderer.Visible(1,1,3,1,1));
                        }
                        // Submit both slots before either producer signal exists.
                        renderer.render(target,packet);
                    }
                    for(int slot=0;slot<2;slot++) {
                        int sequence=pair*2+slot+1;
                        producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true,sequence==1?3:0);
                        producer.copyAndCheckImage(output,sequence,32,32,(x,y)->{
                            int[] clear={0,0,0},atlas={32,64,128},dark={4,16,64},white={255,255,255};
                            if(sequence==10 || sequence==12 || (sequence>=7 && sequence<=9 && (y<8 || y>=24))) return clear;
                            if(sequence==11 || sequence<=2) return x<16?atlas:white;
                            if(sequence<=4) return x<16?dark:white;
                            if(sequence<=7) return x<16?white:dark;
                            if(sequence==8) return x<16?atlas:dark;
                            return x<16?atlas:clear;
                        });
                    }
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void visibleSubmeshMasksKeepHighestBitAndAllElements(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                var ranges=new ArrayList<NativeRenderer.Submesh>();
                ranges.add(new NativeRenderer.Submesh(0,6,0));
                for(int i=1;i<31;i++) ranges.add(new NativeRenderer.Submesh(0,0,0));
                ranges.add(new NativeRenderer.Submesh(6,6,0));
                for(int sequence=1;sequence<=4;sequence++) {
                    var packet=frame(1,sequence,sequence);
                    packet.changedTextures=sequence==1?3:0;
                    if(sequence==1) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0,4,5,6,6,7,4}));
                        packet.meshes=List.of(new NativeRenderer.Mesh(1,twoQuads(1),1,8,ranges));
                        packet.materials=List.of(material(1));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                    }
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.visible=sequence==4
                        ?List.of(new NativeRenderer.Visible(1,1|(1<<31),0,0,1))
                        :List.of(new NativeRenderer.Visible(1,sequence==1?1:sequence==2?1<<31:-1,0,0,1));
                    renderer.render(target,packet);
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true,packet.changedTextures);
                    final int selected=sequence;
                    producer.copyAndCheckImage(output,sequence,32,32,(x,y)->
                        selected>=3 || (selected==1?x<16:x>=16)?new int[]{32,64,128}:new int[]{0,0,0});
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void visibleMasksKeepOrderAndInstanceOverrides(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                var vertices=ByteBuffer.allocate(224).order(ByteOrder.nativeOrder());
                vertices.put(quad(0)).put(quad(0));
                for(int v=0;v<8;v++) vertices.putInt(v*28+12,v<4?0xff0000ff:0xff00ff00);
                var red=new NativeRenderer.Visible(1,1,0,1,1);
                var green=new NativeRenderer.Visible(1,2,0,1,1);
                var all=new NativeRenderer.Visible(1,-1,0,1,1);
                for(int pair=0;pair<5;pair++) {
                    for(int slot=0;slot<2;slot++) {
                        int sequence=pair*2+slot+1;
                        var packet=frame(1,sequence,sequence);
                        packet.changedTextures=sequence==1?3:0;
                        if(sequence==1) {
                            packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0,4,5,6,6,7,4}));
                            packet.meshes=List.of(new NativeRenderer.Mesh(1,vertices.array(),1,8,
                                List.of(new NativeRenderer.Submesh(0,6,0),new NativeRenderer.Submesh(6,6,0))));
                            packet.materials=List.of(material(1));
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                        }
                        packet.instances=List.of(new NativeRenderer.Instance(100,100,0,1),new NativeRenderer.Instance(0,0,0,1));
                        packet.visible=switch(sequence) {
                            case 1,10 -> List.of(new NativeRenderer.Visible(1,0b11,0,1,1));
                            case 2 -> List.of(green,red);
                            case 3 -> List.of(red,green,red);
                            case 4 -> List.of(all,red);
                            case 5 -> List.of(red,all);
                            case 6 -> List.of(new NativeRenderer.Visible(1,1,0,0,1),green);
                            case 7 -> List.of(new NativeRenderer.Visible(1,0b11,0,0,2));
                            case 8 -> List.of(new NativeRenderer.Visible(1,1,1,1,1),green);
                            default -> List.of();
                        };
                        renderer.render(target,packet);
                    }
                    for(int slot=0;slot<2;slot++) {
                        int sequence=pair*2+slot+1;
                        producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true,sequence==1?3:0);
                        int[] expected=sequence==9?new int[]{0,0,0}
                            :sequence==2 || sequence==3 || sequence==4 || sequence==8?new int[]{32,0,0}:new int[]{0,64,0};
                        producer.copyAndCheckSolid(output,sequence,32,32,expected);
                    }
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rejectsZeroAndOutOfRangeElementMasks(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=environment.createRuntime(null)) {
            long lastGeneration=NativeRenderer.nextFrameId();
            for(int mask:new int[]{0,2}) {
                try(var renderer=runtime.createRenderer(NativeTestSupport.shaders());
                    var textures=renderer.createSharedTextures(textures(false));
                    var target=renderer.createTarget(8,8)) {
                    long epoch=renderer.nextEpoch(); assertTrue(epoch>lastGeneration);
                    lastGeneration=NativeRenderer.nextFrameId();
                    var packet=frame(epoch,1,1);
                    packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                    packet.meshes=List.of(mesh(1,quad(0))); packet.materials=List.of(material(1));
                    packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.visible=List.of(new NativeRenderer.Visible(1,mask,0,0,1));
                    var failure=assertThrows(IllegalStateException.class,()->renderer.render(target,packet));
                    assertTrue(failure.getMessage().contains("Invalid visible SOLID submesh"),failure.getMessage());
                }
            }
        }
    }
    private static byte[] twoQuads(float height) {
        var result=ByteBuffer.allocate(224).order(ByteOrder.nativeOrder());
        for(int side=0;side<2;side++) {
            var source=ByteBuffer.wrap(quad(0)).order(ByteOrder.nativeOrder());
            for(int v=0;v<4;v++) {
                source.putFloat(v*28,source.getFloat(v*28)*.5f+(side==0?-.5f:.5f));
                source.putFloat(v*28+4,source.getFloat(v*28+4)*height);
            }
            result.put(source.array());
        }
        return result.array();
    }
    @ParameterizedTest @MethodSource("paths")
    void incrementalAdditionsKeepResidentBatches(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                for(int pair=0;pair<4;pair++) {
                    for(int slot=0;slot<2;slot++) {
                        int sequence=pair*2+slot+1;
                        var packet=frame(sequence==8?2:1,sequence,sequence);
                        packet.changedTextures=sequence==1?3:0;
                        if(sequence==1 || sequence==8) {
                            packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                            packet.meshes=List.of(mesh(1,quad(0))); packet.materials=List.of(material(1));
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                        }
                        if(sequence==2) {
                            var indices=new ArrayList<NativeRenderer.Index>(); var meshes=new ArrayList<NativeRenderer.Mesh>();
                            var materials=new ArrayList<NativeRenderer.Material>(); var primitives=new ArrayList<NativeRenderer.Primitive>();
                            // Grow every resident dictionary while the original batch is still in flight.
                            for(long id=2;id<=97;id++) {
                                indices.add(new NativeRenderer.Index(id,new int[]{0,1,2,2,3,0}));
                                meshes.add(new NativeRenderer.Mesh(id,quad(0),id,4,List.of(new NativeRenderer.Submesh(0,6,0))));
                                materials.add(new NativeRenderer.Material(id,1,1,1)); primitives.add(new NativeRenderer.Primitive(id,id,id));
                            }
                            packet.indices=indices; packet.meshes=meshes; packet.materials=materials; packet.primitives=primitives;
                        }
                        if(sequence==4) packet.materials=List.of(new NativeRenderer.Material(1,0,0,1));
                        if(sequence==5) {
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,97,97));
                            packet.removedMaterials=packet.removedMeshes=packet.removedIndices=new long[]{1};
                        }
                        if(sequence==6) {
                            packet.removedPrimitives=packet.removedMaterials=packet.removedMeshes=packet.removedIndices=
                                java.util.stream.LongStream.range(2,97).toArray();
                        }
                        packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                        packet.visible=List.of(new NativeRenderer.Visible(sequence==3 || sequence==7?97:1,1,0,0,1));
                        renderer.render(target,packet);
                    }
                    for(int slot=0;slot<2;slot++) {
                        int sequence=pair*2+slot+1;
                        producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true,sequence==1?3:0);
                        int[] expected=sequence==4?new int[]{4,16,64}:sequence==1 || sequence==2 || sequence==8?
                            new int[]{32,64,128}:new int[]{255,255,255};
                        producer.copyAndCheckSolid(output,sequence,32,32,expected);
                    }
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void samplesAllMipsThroughBothSharingPaths(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
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
                    var packet=frame(1,sequence,sequence);
                    packet.indices=frame==0?List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0})):List.of();
                    packet.materials=frame==0?List.of(material(1)):List.of();
                    packet.meshes=List.of(mesh(sequence,quad(mip)));
                    packet.primitives=List.of(new NativeRenderer.Primitive(sequence,sequence,1));
                    packet.visible=List.of(new NativeRenderer.Visible(sequence,1,0,0,1));
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.removedMeshes=packet.removedPrimitives=frame==0?new long[0]:new long[]{sequence-1};
                    renderer.render(preview,packet);
                    producer.copyAndCheckSolid(output,sequence,32,32,new int[]{32+mip*32,64+mip*16,128});
                }
                producer.writeTerrainInputs(inputs,split,17,9,5,11);
                renderer.render(preview,frame(1,11,11));
                producer.copyAndCheckSolid(output,11,32,32,new int[]{0,0,0});
            } finally { for(var image:inputs) image.close(); }
        }
    }
    @ParameterizedTest @MethodSource("paths")
    void reversedDepthEpochAndLargeCoordinates(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
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
                    var packet=frame(number<3?1:2,number,number);
                    packet.cameraBlock=camera; packet.flags|=number%2==0?NativeRenderer.RGSS:0;
                    if(number==1 || number==4) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.materials=List.of(material(number==1?4:1));
                        packet.meshes=List.of(mesh(1,near),mesh(2,far));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1),new NativeRenderer.Primitive(2,2,1));
                    }
                    if(number!=3) {
                        packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1),new NativeRenderer.Visible(2,1,0,1,1));
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
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders())) {
            for(int generation=0;generation<3;generation++) {
                boolean split=generation%2==1;
                try(var textures=renderer.createSharedTextures(textures(split));
                    var target=renderer.createTarget(32+generation*4,32+generation*4);
                    var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
                    var imports=new ArrayList<VulkanPreviewImage>();
                    try {
                        for(var e:textures.exports()) imports.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                        var packet=frame(1,1,1);
                        if(generation==0) {
                            packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                            packet.meshes=List.of(mesh(1,quad(0))); packet.materials=List.of(material(1));
                            packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                        }
                        packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                        packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1));
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
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders())) {
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
    // Four meshes fill the first geometry block; the last two share the second block at a nonzero base vertex.
    @ParameterizedTest @MethodSource("paths")
    void multipleGeometryBlocksKeepBaseVertices(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                byte[][] vertices={span(500_000,-.75f,.18f),span(600_000,-.25f,.18f),span(200_000,.25f,.18f),span(200_000,.75f,.18f)};
                for(int sequence=1;sequence<=4;++sequence) {
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true);
                    var packet=frame(1,sequence,sequence);
                    if(sequence==1) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.meshes=List.of(mesh(1,vertices[0]),mesh(2,vertices[1]),mesh(3,vertices[2]),mesh(4,vertices[3]));
                        packet.materials=List.of(material(1));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1),new NativeRenderer.Primitive(2,2,1),
                            new NativeRenderer.Primitive(3,3,1),new NativeRenderer.Primitive(4,4,1));
                    }
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1),new NativeRenderer.Visible(2,1,1,0,1),
                        new NativeRenderer.Visible(3,1,2,0,1),new NativeRenderer.Visible(4,1,3,0,1));
                    renderer.render(target,packet);
                    producer.copyAndCheckImage(output,sequence,32,32,(x,y)->y>=13 && y<19 &&
                        ((x>=1 && x<7) || (x>=9 && x<15) || (x>=17 && x<23) || (x>=25 && x<31)) ? atlas() : clear());
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    // One frame uploads more than the initial 8 MiB staging capacity; later frames reuse the grown slot buffer.
    @ParameterizedTest @MethodSource("paths")
    void stagingGrowsBeyondInitialCapacity(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                byte[] geometry=span(400_000,0,.25f); // 11.2 MiB, above the prebuilt staging size
                for(int sequence=1;sequence<=4;++sequence) {
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true);
                    var packet=frame(1,sequence,sequence);
                    if(sequence==1) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.meshes=List.of(mesh(1,geometry)); packet.materials=List.of(material(1));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
                    }
                    if(sequence==3) packet.meshes=List.of(mesh(1,geometry)); // Replace on the other slot's buffer.
                    packet.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                    packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1));
                    renderer.render(target,packet);
                    producer.copyAndCheckImage(output,sequence,32,32,(x,y)->x>=12 && x<20 && y>=12 && y<20 ? atlas() : clear());
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }
    // A removed mesh's range is reused only after both in-flight slots released it; the neighbour keeps its range.
    @ParameterizedTest @MethodSource("paths")
    void geometryRangeReuseAfterMeshRemoval(NativeEnvironment.Backend backend,boolean split) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var producer=new VulkanConsumer(backend.name());
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(producer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var textures=renderer.createSharedTextures(textures(split));
            var target=renderer.createTarget(32,32);
            var output=VulkanPreviewImage.create(producer.device,producer.family,target.info())) {
            var inputs=new ArrayList<VulkanPreviewImage>();
            try {
                for(var e:textures.exports()) inputs.add(VulkanPreviewImage.create(producer.device,producer.family,e.image(),e.mipCount(),true));
                byte[] left=span(10_000,-.5f,.25f),right=span(10_000,.5f,.25f);
                for(int sequence=1;sequence<=9;++sequence) {
                    producer.writeTerrainInputs(inputs,split,17,9,5,sequence,true);
                    var packet=frame(1,sequence,sequence);
                    if(sequence==1) {
                        packet.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
                        packet.meshes=List.of(mesh(1,left),mesh(2,right)); packet.materials=List.of(material(1));
                        packet.primitives=List.of(new NativeRenderer.Primitive(1,1,1),new NativeRenderer.Primitive(2,2,1));
                    }
                    if(sequence==4) { packet.removedPrimitives=new long[]{1}; packet.removedMeshes=new long[]{1}; }
                    if(sequence==7) {
                        packet.meshes=List.of(mesh(3,left)); // Same size as the removed mesh: reuses its released range.
                        packet.primitives=List.of(new NativeRenderer.Primitive(3,3,1));
                    }
                    boolean leftVisible=sequence<=3 || sequence>=7;
                    packet.instances=leftVisible?List.of(new NativeRenderer.Instance(0,0,0,1),new NativeRenderer.Instance(0,0,0,1))
                        :List.of(new NativeRenderer.Instance(0,0,0,1));
                    if(sequence<=3) packet.visible=List.of(new NativeRenderer.Visible(1,1,0,0,1),new NativeRenderer.Visible(2,1,1,1,1));
                    else if(sequence<=6) packet.visible=List.of(new NativeRenderer.Visible(2,1,0,0,1));
                    else packet.visible=List.of(new NativeRenderer.Visible(3,1,0,0,1),new NativeRenderer.Visible(2,1,1,1,1));
                    renderer.render(target,packet);
                    int[] leftExpected=leftVisible?atlas():clear();
                    producer.copyAndCheckImage(output,sequence,32,32,(x,y)->{
                        if(y<12 || y>=20) return clear();
                        if(x>=4 && x<12) return leftExpected;
                        if(x>=20 && x<28) return atlas();
                        return clear();
                    });
                }
            } finally { for(var image:inputs) image.close(); }
        }
    }

}
