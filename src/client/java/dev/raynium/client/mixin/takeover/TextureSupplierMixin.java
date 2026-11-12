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
@Mixin(targets={"net.minecraft.client.renderer.texture.DynamicTexture","net.minecraft.client.renderer.texture.TextureAtlas","net.minecraft.client.gui.font.FontTexture","net.minecraft.client.renderer.texture.ReloadableTexture","net.minecraft.client.renderer.texture.MipmappedTexture","net.minecraft.client.resources.palette.PalettedTextureManager$AtlasTexture"})
abstract class TextureSupplierMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createTexture(Ljava/util/function/Supplier;ILcom/mojang/renderpearl/api/GpuFormat;IIII)Lcom/mojang/renderpearl/api/textures/GpuTexture;"))
    private static GpuTexture raynium$texture(GpuDevice device,Supplier<String> label,int usage,GpuFormat format,int width,int height,int layers,int mips,Operation<GpuTexture> original) { return TakeoverHandles.texture(label==null?null:label.get(),usage,format,width,height,layers,mips); }
}
