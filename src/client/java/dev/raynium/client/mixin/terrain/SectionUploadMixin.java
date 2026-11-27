package dev.raynium.client.mixin.terrain;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.mojang.blaze3d.vertex.StagingBuffer;
import com.mojang.blaze3d.vertex.UberGpuBuffer;
import com.mojang.renderpearl.api.device.GpuDevice;
import dev.raynium.client.terrain.NoopTerrainStagingBuffer;
import dev.raynium.client.terrain.TerrainCapture;
import dev.raynium.client.terrain.TerrainDrain;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(SectionRenderDispatcher.class)
abstract class SectionUploadMixin {
    @WrapOperation(method = "<init>", at = @At(value = "INVOKE", target = "Lcom/mojang/blaze3d/vertex/StagingBuffer;create(Ljava/lang/String;Lcom/mojang/renderpearl/api/device/GpuDevice;I)Lcom/mojang/blaze3d/vertex/StagingBuffer;"))
    private StagingBuffer raynium$stagingBuffer(String name, GpuDevice device, int size, Operation<StagingBuffer> original) {
        if (TerrainCapture.active())
            return new NoopTerrainStagingBuffer();
        return original.call(name, device, size);
    }

    @WrapOperation(method = "uploadTerrainBuffersToGpu", at = @At(value = "INVOKE", target = "Lcom/mojang/blaze3d/vertex/UberGpuBuffer;uploadStagedAllocations(Lcom/mojang/renderpearl/api/device/GpuDevice;Lcom/mojang/blaze3d/vertex/StagingBuffer$Uploader;)Z"))
    private boolean raynium$drain(UberGpuBuffer<?> buffer, GpuDevice device, StagingBuffer.Uploader uploader, Operation<Boolean> original) {
        if (!TerrainCapture.active())
            return original.call(buffer, device, uploader);
        ((TerrainDrain)buffer).raynium$drain(uploader);
        return false;
    }
}
