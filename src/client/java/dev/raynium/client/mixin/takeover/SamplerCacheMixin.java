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
@Mixin(targets={"com.mojang.blaze3d.systems.SamplerCache"})
abstract class SamplerCacheMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createSampler(Lcom/mojang/renderpearl/api/textures/AddressMode;Lcom/mojang/renderpearl/api/textures/AddressMode;Lcom/mojang/renderpearl/api/textures/FilterMode;Lcom/mojang/renderpearl/api/textures/FilterMode;ILjava/util/OptionalDouble;)Lcom/mojang/renderpearl/api/textures/GpuSampler;"))
    private static GpuSampler raynium$sampler(GpuDevice device,AddressMode u,AddressMode v,FilterMode min,FilterMode mag,int anisotropy,OptionalDouble lod,Operation<GpuSampler> original) { return new TakeoverHandles.Sampler(u,v,min,mag,anisotropy,lod); }
}
