package dev.raynium.client.mixin.takeover;

import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.takeover.TakeoverHandles;
import java.lang.foreign.MemorySegment;
import net.minecraft.client.gui.font.providers.BitmapProvider;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(targets="net.minecraft.client.gui.font.providers.BitmapProvider$Glyph$1")
abstract class BitmapGlyphUploadMixin {
    @Shadow @Final private BitmapProvider.Glyph this$0;
    @Inject(method="upload",at=@At("HEAD"),cancellable=true)
    private void raynium$upload(int x,int y,GpuTexture texture,CallbackInfo ci) {
        var glyph=this$0;
        var image=glyph.imageData().image;
        int pitch=image.getWidth()*image.format().components();
        long offset=(long)glyph.offsetY()*pitch+(long)glyph.offsetX()*image.format().components();
        var pixels=MemorySegment.ofBuffer(image.getPixelBytes()).asSlice(offset);
        ((TakeoverHandles.Texture)texture).nativeTexture().upload(0,x,y,glyph.width(),glyph.height(),pitch,pixels);
        ci.cancel();
    }
}
