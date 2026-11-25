package dev.raynium.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.device.GpuDevice;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import java.util.function.Supplier;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(targets="net.minecraft.client.renderer.MappableRingBuffer")
abstract class MappableRingBufferMixin {
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createBuffer(Ljava/util/function/Supplier;IJ)Lcom/mojang/renderpearl/api/buffers/GpuBuffer;"))
    private GpuBuffer raynium$allocate(GpuDevice device,Supplier<String> label,int usage,long size,Operation<GpuBuffer> original) {
        if(device instanceof RayniumGpuDevice raynium) return raynium.createFenceRotatedBuffer(usage,size,dev.raynium.bridge.NativeBuffer.WriteSource.MAPPABLE_RING);
        return original.call(device,label,usage,size);
    }
}
