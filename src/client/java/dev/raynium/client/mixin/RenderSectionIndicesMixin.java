package dev.raynium.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.mojang.blaze3d.vertex.UberGpuBuffer;
import dev.raynium.client.terrain.*;
import java.nio.ByteBuffer;
import net.minecraft.client.renderer.chunk.*;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(SectionRenderDispatcher.RenderSection.class)
abstract class RenderSectionIndicesMixin {
    @WrapOperation(method="addSectionBuffersToUberBuffer",at=@At(value="INVOKE",
        target="Lcom/mojang/blaze3d/vertex/UberGpuBuffer;addAllocation(Ljava/lang/Object;Lcom/mojang/blaze3d/vertex/UberGpuBuffer$UploadCallback;Ljava/nio/ByteBuffer;)Z",ordinal=1))
    private boolean raynium$indices(UberGpuBuffer<?> buffer,Object allocationKey,UberGpuBuffer.UploadCallback<CompiledSectionMesh> callback,
        ByteBuffer source,Operation<Boolean> original,ChunkSectionLayer layer,CompiledSectionMesh mesh,ByteBuffer vertices,ByteBuffer indices) {
        var section=((PayloadOwner)(Object)mesh).raynium$payload();
        if(!TerrainCapture.active() || layer!=ChunkSectionLayer.TRANSLUCENT || section==null || section.epoch!=TerrainCapture.epoch())
            return original.call(buffer,allocationKey,callback,source);
        var payload=section.layers[2];
        var snapshot=payload.copyIndices(source,mesh.getSectionDraw(layer).indexType().bytes);
        UberGpuBuffer.UploadCallback<CompiledSectionMesh> adopted=value->{
            payload.indices=snapshot;
            callback.bufferHasBeenUploaded(value);
        };
        return original.call(buffer,allocationKey,adopted,source);
    }
}
