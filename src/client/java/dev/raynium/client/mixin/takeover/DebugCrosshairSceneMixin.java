package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.device.GpuDevice;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.takeover.TakeoverRuntime;
import dev.raynium.client.takeover.scene.*;
import java.nio.ByteBuffer;
import java.util.function.Supplier;
import net.minecraft.client.renderer.DebugCrosshairRenderer;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.state.GameRenderState;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(DebugCrosshairRenderer.class)
abstract class DebugCrosshairSceneMixin implements SceneEnvironmentSource {
    @Shadow @Final private GpuBuffer crosshairBuffer;
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createBuffer(Ljava/util/function/Supplier;ILjava/nio/ByteBuffer;)Lcom/mojang/renderpearl/api/buffers/GpuBuffer;"))
    private static GpuBuffer raynium$geometry(GpuDevice device,Supplier<String> label,int usage,ByteBuffer bytes,Operation<GpuBuffer> original) {
        return new SceneGeometry(usage,bytes);
    }
    @Override public void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view,boolean oit) {
        var camera=state.levelRenderState.cameraRenderState; var frame=TakeoverRuntime.frameBuffer().scene();
        var geometry=(SceneGeometry)crosshairBuffer; int indices=geometry.indices(PrimitiveTopology.LINES,24);
        float scale=.01f*state.windowRenderState.guiScale;
        var transform=new Matrix4f(view).translate(0,0,-1).rotateX((float)Math.toRadians(camera.xRot))
            .rotateY((float)Math.toRadians(camera.yRot)).scale(-scale,scale,-scale);
        var uniforms=new SceneUniforms(frame,state,projection,view,net.minecraft.world.level.CardinalLighting.Type.DEFAULT); uniforms.noFog();
        int constants=uniforms.transforms(transform,new Matrix4f());
        for(int i=0;i<2;i++) {
            var program=ScenePrograms.require(i==0?RenderPipelines.LINES:RenderPipelines.LINES_DEPTH_BIAS);
            int first=frame.bindingCount(); uniforms.bind(program,constants);
            frame.draw(program.id(),NativeSceneFrame.HAND,geometry.reference(),indices,24,18,4,first,1,-1,0,i*18L*4,0,
                0,0,state.windowRenderState.width,state.windowRenderState.height);
        }
    }
}
