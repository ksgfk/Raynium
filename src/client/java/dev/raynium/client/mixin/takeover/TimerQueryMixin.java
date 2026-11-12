package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.systems.TimerQuery;
import com.mojang.renderpearl.api.commands.GpuQueryPool;
import com.mojang.renderpearl.api.device.GpuDevice;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(TimerQuery.class)
abstract class TimerQueryMixin {
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createTimestampQueryPool(I)Lcom/mojang/renderpearl/api/commands/GpuQueryPool;"))
    private GpuQueryPool raynium$pool(GpuDevice device,int count,Operation<GpuQueryPool> original) { return null; }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;getDevice()Lcom/mojang/renderpearl/api/device/GpuDevice;"))
    private GpuDevice raynium$device(Operation<GpuDevice> original) { return null; }
    @Inject(method={"beginProfile","endProfile","close"},at=@At("HEAD"),cancellable=true)
    private void raynium$noQueries(CallbackInfo ci) { ci.cancel(); }
}
