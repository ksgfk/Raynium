package dev.raynium.bridge;

import dev.raynium.client.takeover.scene.ScenePrograms;
import dev.raynium.client.takeover.scene.VanillaPipelineCatalog;
import java.lang.foreign.Arena;
import java.nio.file.Path;
import java.util.ArrayList;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

/** Exercises every compiled variant's layout and attachments; scene captures verify actual imagery. */
@Tag("native")
class NativePipelineCatalogTest {
    @TempDir Path gameDirectory;

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void createsAndSubmitsEveryAvailableVanillaVariant(NativeEnvironment.Backend backend) throws Exception {
        var messages=new ArrayList<String>();
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(messages));
            var window=new NativeSwapchainTest.Window(); var runtime=environment.createRuntime();
            var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var chain=renderer.createSwapchain(window.hwnd,64,64,NativeSwapchain.PresentMode.IMMEDIATE);
            var texture=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
            var cube=renderer.createTexture(1,1,1,6,NativeTexture.Format.RGBA8,NativeTexture.CUBE);
            var texels=renderer.createSceneBuffer(256); var arena=Arena.ofConfined()) {
            var zeros=arena.allocate(4096,256); texels.upload(0,zeros.asSlice(0,256));
            texture.upload(0,0,0,1,1,4,zeros.asSlice(0,4));
            for(int layer=0;layer<6;layer++) cube.uploadLayer(0,layer,0,0,1,1,4,zeros.asSlice(0,4));
            long sequence=0;
            for(var entry:VanillaPipelineCatalog.create().entries()) {
                if(entry.optional() && entry.identity().polygonMode().name().equals("WIREFRAME") && !runtime.deviceInfo().wireframe()) continue;
                if(entry.names().getFirst().equals("OIT_DEPTH_BOUNDS_CULL")) continue; // Native OIT owns the bounds/depth cull stage.
                System.out.println("Pipeline probe: "+entry.names().getFirst());
                var colors=entry.identity().colors();
                boolean depthOnly=colors.isEmpty();
                if(!depthOnly) { depthOnly=true; for(var color:colors) depthOnly&=color.writeMask()==0; }
                String format=depthOnly?"":colors.getFirst().format().name();
                int phase=depthOnly?NativeSceneFrame.WATER_MASK:format.equals("RGBA32_FLOAT")?NativeSceneFrame.OIT_BOUNDS:
                    format.equals("RGBA16_FLOAT")?(colors.size()==2?NativeSceneFrame.OIT_TRANSMITTANCE:NativeSceneFrame.OIT_ACCUMULATE):NativeSceneFrame.SOLID;
                var program=ScenePrograms.require(entry.pipeline());
                if(program.constants().containsKey("CloudInfo") && phase>=NativeSceneFrame.OIT_BOUNDS && phase<=NativeSceneFrame.OIT_ACCUMULATE) phase+=4;
                for(int repeat=0;repeat<3;repeat++) {
                    var frame=NativeTestSupport.solidFrame(++sequence); frame.flags=NativeRenderer.IMPROVED_TRANSPARENCY;
                    var buffer=renderer.frameBuffer(); buffer.begin(0,0,0,0,0); var scene=buffer.scene();
                    int vertices=scene.borrow(zeros),first=scene.bindingCount();
                    for(var constant:program.constants().values()) {
                        int bytes=(constant.size()+255)&~255;
                        assertTrue(bytes<=zeros.byteSize(),entry.names().toString());
                        scene.constant(constant.binding(),scene.copy(zeros.asSlice(0,bytes)),0,bytes);
                    }
                    for(var input:program.textures().entrySet()) {
                        int resource=switch(input.getKey()) { case "DepthBoundsSampler"->1; case "Coeff0"->2; case "Coeff1"->3; default->0; };
                        if(resource!=0 && ((phase>=NativeSceneFrame.OIT_BOUNDS && phase<=NativeSceneFrame.OIT_ACCUMULATE) ||
                            (phase>=NativeSceneFrame.CLOUD_BOUNDS && phase<=NativeSceneFrame.CLOUD_ACCUMULATE)))
                            scene.graphTexture(input.getValue().texture(),resource);
                        else scene.texture(input.getValue().texture(),entry.names().getFirst().equals("PANORAMA")?cube.id():texture.id(),32);
                    }
                    for(int binding:program.texels().values()) scene.signedByteBuffer(binding,texels.reference(),0,256);
                    int count=entry.identity().topology().name().equals("TRIANGLES")?3:4;
                    scene.draw(entry.id(),phase,vertices,-1,count,0,0,first,1,vertices,0,0,0,0,0,64,64);
                    try { renderer.presentPrepared(chain,frame); }
                    catch(RuntimeException failure) { throw new AssertionError("Vanilla pipeline "+entry.names(),failure); }
                }
            }
            assertTrue(sequence>=570,"Expected the complete current vanilla catalog");
        }
        assertFalse(messages.stream().anyMatch(message->message.contains("VUID-") || message.contains("[critical]")),messages.toString());
    }
}
