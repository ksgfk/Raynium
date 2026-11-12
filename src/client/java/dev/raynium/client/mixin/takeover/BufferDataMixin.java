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
@Mixin(targets={"net.minecraft.client.renderer.fog.FogRenderer","net.minecraft.client.renderer.CubeMap","net.minecraft.client.renderer.DebugCrosshairRenderer"})
abstract class BufferDataMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createBuffer(Ljava/util/function/Supplier;ILjava/nio/ByteBuffer;)Lcom/mojang/renderpearl/api/buffers/GpuBuffer;"))
    private static GpuBuffer raynium$buffer(GpuDevice device,Supplier<String> label,int usage,ByteBuffer data,Operation<GpuBuffer> original) { return TakeoverHandles.buffer(label,usage,data); }
}
