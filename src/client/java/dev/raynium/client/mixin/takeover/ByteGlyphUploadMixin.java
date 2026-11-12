package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.takeover.TakeoverHandles;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(targets="net.minecraft.client.gui.font.providers.UnihexProvider$Glyph$2")
abstract class ByteGlyphUploadMixin {
    @WrapOperation(method="upload",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;writeToTexture(Lcom/mojang/renderpearl/api/textures/GpuTexture;Ljava/nio/ByteBuffer;IIIIII)V"))
    private void raynium$upload(CommandEncoder encoder,GpuTexture texture,ByteBuffer pixels,int mip,int layer,int x,int y,int width,int height,Operation<Void> original) {
        var target=((TakeoverHandles.Texture)texture).nativeTexture();
        target.upload(mip,x,y,width,height,width*4,MemorySegment.ofBuffer(pixels));
    }
}
