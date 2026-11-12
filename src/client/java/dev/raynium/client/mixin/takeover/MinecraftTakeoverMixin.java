package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.expression.Definition;
import com.llamalad7.mixinextras.expression.Expression;
import com.llamalad7.mixinextras.injector.ModifyExpressionValue;
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.device.*;
import dev.raynium.client.takeover.TakeoverRuntime;
import net.minecraft.client.Minecraft;
import net.minecraft.client.PreferredGraphicsApi;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.systems.*;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import java.util.*;
import net.minecraft.client.renderer.*;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import com.llamalad7.mixinextras.sugar.Share;
import com.llamalad7.mixinextras.sugar.ref.LocalRef;
import dev.raynium.client.diagnostics.ClientFrameEvent;
import dev.raynium.client.takeover.UnsupportedCapture;
import net.minecraft.network.chat.Component;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(Minecraft.class)
abstract class MinecraftTakeoverMixin {
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lnet/minecraft/client/PreferredGraphicsApi;getBackendsToTry()[Lcom/mojang/renderpearl/api/device/GpuBackend;"))
    private GpuBackend[] raynium$backends(PreferredGraphicsApi api,Operation<GpuBackend[]> original) { return new GpuBackend[0]; }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/backend/vulkan/VulkanBackend;checkBackendAvailable()Lcom/mojang/renderpearl/api/device/BackendCreationException;"))
    private BackendCreationException raynium$skipProbe(Operation<BackendCreationException> original) { return null; }
    @Definition(id="device",local=@Local(type=GpuDevice.class))
    @Expression("device == null")
    @ModifyExpressionValue(method="<init>",at=@At("MIXINEXTRAS:EXPRESSION"))
    private boolean raynium$deviceAbsent(boolean original) { return false; }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;initRenderer(Lcom/mojang/renderpearl/api/device/GpuDevice;)V"))
    private void raynium$initializeSamplers(GpuDevice device,Operation<Void> original) { RenderSystem.getSamplerCache().initialize(); }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;getDeviceInfo()Lcom/mojang/renderpearl/api/device/DeviceInfo;"))
    private DeviceInfo raynium$deviceInfo(GpuDevice device,Operation<DeviceInfo> original) { return TakeoverRuntime.deviceInfo(); }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createSurface(JLjava/util/function/BooleanSupplier;)Lcom/mojang/renderpearl/api/device/GpuSurface;"))
    private GpuSurface raynium$surface(GpuDevice device,long window,java.util.function.BooleanSupplier iconified,Operation<GpuSurface> original) {
        TakeoverRuntime.createSurface(window); return null;
    }
    @WrapOperation(method="close",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;close()V"))
    private void raynium$close(GpuSurface surface,Operation<Void> original) { TakeoverRuntime.close(); }
    @Inject(method="renderFrame",at=@At("HEAD"))
    private void raynium$beginFrame(CallbackInfo ci,@Share("raynium$frame") LocalRef<ClientFrameEvent> event) {
        event.set(ClientFrameEvent.start(((Minecraft)(Object)this).level!=null));
        dev.raynium.client.terrain.TerrainCapture.beginFrame();
    }
    @Inject(method="renderFrame",at=@At("RETURN"))
    private void raynium$endFrame(CallbackInfo ci,@Share("raynium$frame") LocalRef<ClientFrameEvent> event) {
        var frame=event.get();
        if(frame!=null) { frame.end(); frame.commit(); }
    }
    @ModifyExpressionValue(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/jtracy/TracyClient;isAvailable()Z"))
    private boolean raynium$noFrameCapture(boolean value) { return false; }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;isAcquired()Z"))
    private boolean raynium$notAcquired(GpuSurface surface,Operation<Boolean> original) { return false; }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;isSuboptimal()Z"))
    private boolean raynium$suboptimal(GpuSurface surface,Operation<Boolean> original) { return TakeoverRuntime.suboptimal(); }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;supportedPresentModes()Ljava/util/Collection;"))
    private Collection<GpuSurface.PresentMode> raynium$modes(GpuSurface surface,Operation<Collection<GpuSurface.PresentMode>> original) { return List.of(GpuSurface.PresentMode.IMMEDIATE,GpuSurface.PresentMode.FIFO); }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;configure(Lcom/mojang/renderpearl/api/device/GpuSurface$Configuration;)V"))
    private void raynium$configure(GpuSurface surface,GpuSurface.Configuration config,Operation<Void> original) { TakeoverRuntime.configure(config); }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;currentConfiguration()Ljava/util/Optional;"))
    private Optional<GpuSurface.Configuration> raynium$config(GpuSurface surface,Operation<Optional<GpuSurface.Configuration>> original) { return TakeoverRuntime.configuration(); }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;acquireNextTexture()V"))
    private void raynium$acquire(GpuSurface surface,Operation<Void> original) {}
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/GameRenderer;render()V"))
    private void raynium$prepare(GameRenderer game,Operation<Void> original) { ((TakeoverRuntime.FrameSource)game).raynium$prepare(); }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;getDevice()Lcom/mojang/renderpearl/api/device/GpuDevice;"))
    private GpuDevice raynium$device(Operation<GpuDevice> original) { return null; }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createCommandEncoder()Lcom/mojang/renderpearl/api/commands/CommandEncoder;"))
    private CommandEncoder raynium$encoder(GpuDevice device,Operation<CommandEncoder> original) { return null; }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;submit()V"))
    private void raynium$submit(CommandEncoder encoder,Operation<Void> original) { TakeoverRuntime.present(); }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;getDynamicUniforms()Lnet/minecraft/client/renderer/DynamicGpuData;"))
    private DynamicGpuData raynium$uniforms(Operation<DynamicGpuData> original) { return null; }
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/DynamicGpuData;reset()V"))
    private void raynium$reset(DynamicGpuData uniforms,Operation<Void> original) {}
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/LevelRenderer;endFrame()V"))
    private void raynium$end(LevelRenderer level,Operation<Void> original) {}
    @Inject(method="grabPanoramixScreenshot",at=@At("HEAD"),cancellable=true)
    private void raynium$unsupported(CallbackInfoReturnable<Component> ci) { UnsupportedCapture.warn(); ci.setReturnValue(Component.literal("Raynium screenshots are unavailable")); }
    @WrapOperation(method="lambda$new$7",at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;getDevice()Lcom/mojang/renderpearl/api/device/GpuDevice;"))
    private GpuDevice raynium$telemetryDevice(Operation<GpuDevice> original) { return null; }
    @WrapOperation(method="lambda$new$7",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;getDeviceInfo()Lcom/mojang/renderpearl/api/device/DeviceInfo;"))
    private DeviceInfo raynium$info(GpuDevice device,Operation<DeviceInfo> original) { return TakeoverRuntime.deviceInfo(); }
}
