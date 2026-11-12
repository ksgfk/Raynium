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
@Mixin(targets={"com.mojang.blaze3d.platform.Lighting","net.minecraft.client.renderer.ProjectionMatrixBuffer","net.minecraft.client.renderer.GlobalSettingsUniform"})
abstract class BufferWriteMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;writeToBuffer(Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Ljava/nio/ByteBuffer;)V"))
    private static void raynium$write(CommandEncoder encoder,GpuBufferSlice slice,ByteBuffer bytes,Operation<Void> original) {}
}
