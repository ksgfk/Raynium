package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import com.mojang.renderpearl.api.device.GpuDevice;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.takeover.TakeoverRuntime;
import dev.raynium.client.takeover.scene.*;
import java.nio.ByteBuffer;
import java.util.function.Supplier;
import net.minecraft.client.renderer.*;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.state.level.WorldBorderRenderState;
import net.minecraft.client.renderer.texture.AbstractTexture;
import net.minecraft.util.ARGB;
import net.minecraft.util.Util;
import net.minecraft.world.phys.Vec3;
import org.joml.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(WorldBorderRenderer.class)
abstract class WorldBorderSceneMixin implements SceneEnvironmentSource {
    @Shadow @Final private GpuBuffer worldBorderBuffer;
    @Shadow private AbstractTexture texture;
    @Shadow private double lastMinX,lastMinZ;
    @Shadow public abstract void prepare(WorldBorderRenderState state,Vec3 camera,double distance,double far);
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createBuffer(Ljava/util/function/Supplier;IJ)Lcom/mojang/renderpearl/api/buffers/GpuBuffer;"))
    private GpuBuffer raynium$buffer(GpuDevice device,Supplier<String> label,int usage,long size,Operation<GpuBuffer> original) { return new SceneGeometry(usage,size); }
    @WrapOperation(method="rebuildWorldBorderBuffer",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createCommandEncoder()Lcom/mojang/renderpearl/api/commands/CommandEncoder;"))
    private CommandEncoder raynium$encoder(GpuDevice device,Operation<CommandEncoder> original) { return null; }
    @WrapOperation(method="rebuildWorldBorderBuffer",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;writeToBuffer(Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Ljava/nio/ByteBuffer;)V"))
    private void raynium$upload(CommandEncoder encoder,GpuBufferSlice slice,ByteBuffer bytes,Operation<Void> original) { ((SceneGeometry)worldBorderBuffer).upload(0,bytes); }
    @WrapOperation(method="prepare",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem$AutoStorageIndexBuffer;requestIndexCount(I)V"))
    private void raynium$indices(RenderSystem.AutoStorageIndexBuffer indices,int count,Operation<Void> original) {}
    @Override public void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view,boolean oit) {
        var border=state.levelRenderState.worldBorderRenderState; if(border.alpha<=0) return;
        var camera=state.levelRenderState.cameraRenderState; double distance=state.optionsRenderState.renderDistance*16;
        prepare(border,camera.pos,distance,camera.depthFar);
        var frame=TakeoverRuntime.frameBuffer().scene(); var geometry=(SceneGeometry)worldBorderBuffer;
        int indices=geometry.indices(PrimitiveTopology.QUADS,16);
        var uniforms=new SceneUniforms(frame,state,projection,view,net.minecraft.world.level.CardinalLighting.Type.DEFAULT);
        float textureOffset=(float)(dev.raynium.client.validation.VisualValidation.animationMillis(Util.getMillis())%3000L)/3000;
        int transforms=uniforms.transforms(view,new Vector4f(ARGB.red(border.tint)/255f,ARGB.green(border.tint)/255f,ARGB.blue(border.tint)/255f,(float)border.alpha),
            new Vector3f((float)(lastMinX-camera.pos.x),(float)-camera.pos.y,(float)(lastMinZ-camera.pos.z)),new Matrix4f().translation(textureOffset,textureOffset,0));
        var sides=border.closestBorder(camera.pos.x,camera.pos.z);
        for(int stage=0;stage<(oit?3:1);stage++) {
            var program=ScenePrograms.require(oit?RenderPipelines.OIT_WORLD_BORDER.getPipeline(OitStage.values()[stage]):RenderPipelines.WORLD_BORDER);
            for(var side:sides) if(side.distance()<distance) {
                int first=frame.bindingCount(); uniforms.bind(program,transforms);
                program.texture(frame,"Sampler0",texture.getTextureView(),texture.getSampler()); if(oit) program.oitTextures(frame);
                frame.draw(program.id(),oit?NativeSceneFrame.OIT_BOUNDS+stage:NativeSceneFrame.AFTER_TERRAIN,geometry.reference(),indices,
                    16,6,4,first,1,-1,0,side.direction().get2DDataValue()*24L,0,0,0,state.windowRenderState.width,state.windowRenderState.height);
            }
        }
    }
}
