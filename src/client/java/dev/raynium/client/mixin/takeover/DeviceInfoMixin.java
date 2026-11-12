package dev.raynium.client.mixin.takeover;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.device.*;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import dev.raynium.client.takeover.*;
import java.nio.ByteBuffer;
import java.util.OptionalDouble;
import java.util.function.Supplier;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
@Mixin(targets={"net.minecraft.client.gui.components.debug.DebugEntrySystemSpecs","net.minecraft.client.Options","net.minecraft.client.GraphicsPreset","net.minecraft.client.renderer.texture.TextureAtlas","com.mojang.blaze3d.platform.Lighting","net.minecraft.client.renderer.LevelRenderer","net.minecraft.client.renderer.GpuWarnlistManager$Preparations","net.minecraft.client.Camera","net.minecraft.client.renderer.Projection","com.mojang.blaze3d.vertex.StagingBuffer"})
abstract class DeviceInfoMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;getDeviceInfo()Lcom/mojang/renderpearl/api/device/DeviceInfo;"))
    private static DeviceInfo raynium$info(GpuDevice device,Operation<DeviceInfo> original) { return TakeoverRuntime.deviceInfo(); }
}
