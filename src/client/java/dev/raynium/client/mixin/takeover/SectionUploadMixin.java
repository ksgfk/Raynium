package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.vertex.*;
import com.mojang.renderpearl.api.device.GpuDevice;
import dev.raynium.client.takeover.TerrainDrain;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(SectionRenderDispatcher.class)
abstract class SectionUploadMixin {
    @WrapOperation(method="uploadTerrainBuffersToGpu",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/vertex/UberGpuBuffer;uploadStagedAllocations(Lcom/mojang/renderpearl/api/device/GpuDevice;Lcom/mojang/blaze3d/vertex/StagingBuffer$Uploader;)Z"))
    private boolean raynium$drain(UberGpuBuffer<?> buffer,GpuDevice device,StagingBuffer.Uploader uploader,Operation<Boolean> original) {
        ((TerrainDrain)buffer).raynium$drain(uploader); return false;
    }
}
