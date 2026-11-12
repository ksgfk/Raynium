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
@Mixin(targets={"net.minecraft.client.renderer.texture.DynamicTexture","net.minecraft.client.renderer.texture.ReloadableTexture"})
abstract class DynamicTextureUploadMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;writeToTexture(Lcom/mojang/renderpearl/api/textures/GpuTexture;Lcom/mojang/blaze3d/platform/NativeImage;)V"))
    private static void raynium$upload(CommandEncoder encoder,GpuTexture texture,NativeImage image,Operation<Void> original) { TakeoverHandles.write(texture,image); }
}
