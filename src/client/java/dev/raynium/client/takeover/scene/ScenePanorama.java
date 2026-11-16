package dev.raynium.client.takeover.scene;

import dev.raynium.bridge.NativeSceneFrame;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.resources.Identifier;
import org.joml.Matrix4f;

public final class ScenePanorama {
    private static final Identifier TEXTURE=Identifier.withDefaultNamespace("textures/gui/title/background/panorama");
    private static final float[] VERTICES={
        -1,-1,1, -1,1,1, 1,1,1, 1,-1,1, 1,-1,1, 1,1,1, 1,1,-1, 1,-1,-1,
        1,-1,-1, 1,1,-1, -1,1,-1, -1,-1,-1, -1,-1,-1, -1,1,-1, -1,1,1, -1,-1,1,
        -1,-1,-1, -1,-1,1, 1,-1,1, 1,-1,-1, -1,1,1, -1,1,-1, 1,1,-1, 1,1,1};
    private static final ByteBuffer CONSTANTS=ByteBuffer.allocate(256).order(ByteOrder.nativeOrder());
    private static SceneGeometry geometry;
    private ScenePanorama() {}
    public static void capture(NativeSceneFrame frame,float spin,int width,int height) {
        var program=ScenePrograms.require(RenderPipelines.PANORAMA);
        var projection=new Matrix4f().setPerspective((float)Math.toRadians(85),(float)width/height,10,.05f,true);
        CONSTANTS.clear(); for(int i=0;i<256;i+=8) CONSTANTS.putLong(i,0);
        SceneUniforms.matrix(projection,CONSTANTS,0); int projectionBlob=frame.copy(MemorySegment.ofBuffer(CONSTANTS));
        SceneUniforms.matrix(new Matrix4f().rotationX((float)Math.PI).rotateX((float)Math.toRadians(10)).rotateY((float)Math.toRadians(spin)),CONSTANTS,0);
        SceneUniforms.matrix(new Matrix4f(),CONSTANTS,64);
        for(int i=0;i<4;i++) CONSTANTS.putFloat(128+i*4,1);
        int transforms=frame.copy(MemorySegment.ofBuffer(CONSTANTS));
        if(geometry==null) {
            var bytes=ByteBuffer.allocate(VERTICES.length*4).order(ByteOrder.nativeOrder());
            for(float value:VERTICES) bytes.putFloat(value); bytes.flip(); geometry=new SceneGeometry(32,bytes);
        }
        int indices=geometry.indices(com.mojang.renderpearl.api.pipeline.PrimitiveTopology.QUADS,24);
        int bindings=frame.bindingCount();
        program.constant(frame,"Projection",projectionBlob); program.constant(frame,"DynamicTransforms",transforms);
        var texture=Minecraft.getInstance().getTextureManager().getTexture(TEXTURE);
        program.texture(frame,"Sampler0",texture.getTextureView(),texture.getSampler());
        frame.draw(program.id(),NativeSceneFrame.PANORAMA,geometry.reference(),indices,24,geometry.indexCount(),4,bindings,1,-1,0,0,0,0,0,width,height);
    }
    public static void close() { if(geometry!=null) { geometry.close(); geometry=null; } }
}
