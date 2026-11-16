package dev.raynium.client.takeover.scene;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.takeover.TakeoverRuntime;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.texture.AbstractTexture;
import org.joml.Matrix4fc;
import org.joml.Vector4fc;

public final class SceneSky {
    public interface Source { void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view); }
    private final NativeSceneFrame frame;
    private final SceneUniforms uniforms;
    private final int width,height;
    public SceneSky(GameRenderState state,Matrix4fc projection,Matrix4fc view) {
        frame=TakeoverRuntime.frameBuffer().scene(); width=state.windowRenderState.width; height=state.windowRenderState.height;
        uniforms=new SceneUniforms(frame,state,projection,view,net.minecraft.world.level.CardinalLighting.Type.DEFAULT);
    }
    public void draw(RenderPipeline pipeline,GpuBuffer buffer,int count,long offset,Matrix4fc transform,Vector4fc color,AbstractTexture texture) {
        var geometry=(SceneGeometry)buffer; var program=ScenePrograms.require(pipeline);
        int indices=geometry.indices(pipeline.getPrimitiveTopology(),count);
        int first=frame.bindingCount(); uniforms.bind(program,uniforms.transforms(transform,color));
        if(texture!=null) program.texture(frame,"Sampler0",texture.getTextureView(),texture.getSampler());
        frame.draw(program.id(),NativeSceneFrame.SKY,geometry.reference(),indices,count,geometry.indexCount(),4,first,1,-1,offset,0,0,0,0,width,height);
    }
}
