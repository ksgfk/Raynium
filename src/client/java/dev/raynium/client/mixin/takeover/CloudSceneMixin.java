package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.*;
import dev.raynium.bridge.NativeSceneBuffer;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.takeover.TakeoverRuntime;
import dev.raynium.client.takeover.scene.*;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import net.minecraft.client.CloudStatus;
import net.minecraft.client.renderer.*;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.util.ARGB;
import net.minecraft.world.phys.Vec3;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.lwjgl.system.MemoryUtil;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(CloudRenderer.class)
abstract class CloudSceneMixin implements SceneEnvironmentSource {
    @Shadow @Final private MappableRingBuffer ubo;
    @Shadow private CloudRenderer.TextureData texture;
    @Shadow private int quadCount;
    @Shadow public abstract void prepare(int color,CloudStatus status,float bottom,int range,Vec3 camera,long time,float partialTicks);
    @Unique private ByteBuffer raynium$faces,raynium$uniform;
    @Unique private NativeSceneBuffer raynium$gpuFaces,raynium$indices;
    @Unique private int raynium$faceBytes,raynium$indexQuads;

    @WrapOperation(method={"prepare(ILnet/minecraft/client/CloudStatus;FILnet/minecraft/world/phys/Vec3;JF)V","endFrame"},
        at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/MappableRingBuffer;rotate()V"))
    private void raynium$rotate(MappableRingBuffer ring,Operation<Void> original) {}
    @WrapOperation(method="prepare(ILnet/minecraft/client/CloudStatus;FILnet/minecraft/world/phys/Vec3;JF)V",
        at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem$AutoStorageIndexBuffer;requestIndexCount(I)V"))
    private void raynium$indices(RenderSystem.AutoStorageIndexBuffer indices,int count,Operation<Void> original) {}
    @WrapOperation(method="prepare(ILnet/minecraft/client/CloudStatus;FILnet/minecraft/world/phys/Vec3;JF)V",
        at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/buffers/GpuBuffer;map(ZZ)Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice$MappedView;"))
    private GpuBufferSlice.MappedView raynium$map(GpuBuffer buffer,boolean read,boolean write,Operation<GpuBufferSlice.MappedView> original) {
        if(buffer==ubo.currentBuffer()) {
            if(raynium$uniform==null) raynium$uniform=MemoryUtil.memCalloc(256);
            return new GpuBufferSlice.MappedView(buffer.slice(),raynium$uniform.clear(),()->{});
        }
        int size=Math.toIntExact(buffer.size());
        if(raynium$faces==null || raynium$faces.capacity()<size) {
            var replacement=MemoryUtil.memAlloc(size);
            NativeSceneBuffer gpu;
            try { gpu=TakeoverRuntime.renderer().createSceneBuffer(size); }
            catch(RuntimeException | Error failure) { MemoryUtil.memFree(replacement); throw failure; }
            if(raynium$gpuFaces!=null) raynium$gpuFaces.close();
            MemoryUtil.memFree(raynium$faces); raynium$faces=replacement; raynium$gpuFaces=gpu;
        }
        raynium$faces.clear().limit(size);
        return new GpuBufferSlice.MappedView(buffer.slice(),raynium$faces,()->{
            raynium$faceBytes=raynium$faces.position();
            raynium$gpuFaces.upload(0,MemorySegment.ofBuffer(raynium$faces.flip()));
        });
    }
    @Override public void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view,boolean oit) {
        var level=state.levelRenderState; var status=state.optionsRenderState.cloudStatus;
        if(status==CloudStatus.OFF || ARGB.alpha(level.cloudColor)==0 || texture==null) return;
        prepare(level.cloudColor,status,level.cloudHeight,state.optionsRenderState.cloudRange,level.cameraRenderState.pos,level.gameTime,level.worldPartialTicks);
        if(quadCount==0) return;
        if(quadCount>raynium$indexQuads) {
            int capacity=Math.max(quadCount,Math.max(256,raynium$indexQuads*2));
            int[] values=new int[Math.multiplyExact(capacity,6)];
            for(int q=0;q<capacity;q++) { int at=q*6,v=q*4; values[at]=v; values[at+1]=v+1; values[at+2]=v+2;
                values[at+3]=v+2; values[at+4]=v+3; values[at+5]=v; }
            var replacement=TakeoverRuntime.renderer().createSceneBuffer(values.length*4L);
            try { replacement.upload(0,MemorySegment.ofArray(values)); }
            catch(RuntimeException | Error failure) { replacement.close(); throw failure; }
            if(raynium$indices!=null) raynium$indices.close(); raynium$indices=replacement; raynium$indexQuads=capacity;
        }
        var frame=TakeoverRuntime.frameBuffer().scene();
        var uniforms=new SceneUniforms(frame,state,projection,view,net.minecraft.world.level.CardinalLighting.Type.DEFAULT);
        uniforms.constant("CloudInfo",MemorySegment.ofBuffer(raynium$uniform.clear()));
        int transforms=uniforms.transforms(view,new Matrix4f());
        for(int stage=0;stage<(oit?3:1);stage++) {
            var pipeline=oit?(status==CloudStatus.FANCY?RenderPipelines.OIT_CLOUDS:RenderPipelines.OIT_FLAT_CLOUDS).getPipeline(OitStage.values()[stage])
                :status==CloudStatus.FANCY?RenderPipelines.CLOUDS:RenderPipelines.FLAT_CLOUDS;
            var program=ScenePrograms.require(pipeline); int first=frame.bindingCount(); uniforms.bind(program,transforms);
            frame.signedByteBuffer(program.texels().get("CloudFaces"),raynium$gpuFaces.reference(),0,raynium$faceBytes);
            if(oit) program.oitTextures(frame);
            frame.draw(program.id(),oit?NativeSceneFrame.CLOUD_BOUNDS+stage:NativeSceneFrame.AFTER_TERRAIN,-1,raynium$indices.reference(),
                quadCount*4,quadCount*6,4,first,1,-1,0,0,0,0,0,state.windowRenderState.width,state.windowRenderState.height);
        }
    }
    @Inject(method="close",at=@At("HEAD"))
    private void raynium$close(CallbackInfo ci) {
        if(raynium$gpuFaces!=null) { raynium$gpuFaces.close(); raynium$gpuFaces=null; }
        if(raynium$indices!=null) { raynium$indices.close(); raynium$indices=null; }
        MemoryUtil.memFree(raynium$faces); raynium$faces=null;
        MemoryUtil.memFree(raynium$uniform); raynium$uniform=null;
    }
}
