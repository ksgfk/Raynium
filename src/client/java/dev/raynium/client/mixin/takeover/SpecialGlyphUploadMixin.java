package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.takeover.TakeoverHandles;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(targets={"net.minecraft.client.gui.font.glyphs.SpecialGlyphs$1","net.minecraft.client.renderer.texture.MipmappedTexture","com.mojang.blaze3d.font.TrueTypeGlyphProvider$Glyph$1","net.minecraft.client.resources.palette.PalettedTextureManager"})
abstract class SpecialGlyphUploadMixin {
    @WrapOperation(method="*",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;writeToTexture(Lcom/mojang/renderpearl/api/textures/GpuTexture;Lcom/mojang/blaze3d/platform/NativeImage;IIII)V"))
    private void raynium$upload(CommandEncoder encoder,GpuTexture texture,NativeImage image,int mip,int layer,int x,int y,Operation<Void> original) {
        TakeoverHandles.write(texture,image,mip,layer,x,y);
    }
}
