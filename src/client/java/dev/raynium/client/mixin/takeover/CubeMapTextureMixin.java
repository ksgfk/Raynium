package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.renderpearl.api.GpuFormat;
import dev.raynium.client.takeover.TakeoverHandles;
import net.minecraft.client.renderer.texture.*;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(CubeMapTexture.class)
abstract class CubeMapTextureMixin extends ReloadableTexture {
    CubeMapTextureMixin() { super(null); }
    @Inject(method="doLoad",at=@At("HEAD"),cancellable=true)
    private void raynium$identity(NativeImage image,CallbackInfo ci) {
        close();
        texture=TakeoverHandles.texture(resourceId().toString(),21,GpuFormat.RGBA8_UNORM,image.getWidth(),image.getHeight()/6,6,1);
        textureView=TakeoverHandles.view(texture,0,1);
        var target=((TakeoverHandles.Texture)texture).nativeTexture();
        int width=image.getWidth(),height=image.getHeight()/6;
        long faceBytes=Math.multiplyExact(Math.multiplyExact((long)width,height),image.format().components());
        var pixels=java.lang.foreign.MemorySegment.ofBuffer(image.getPixelBytes());
        for(int face=0;face<6;face++)
            target.uploadLayer(0,face,0,0,width,height,width*image.format().components(),pixels.asSlice(face*faceBytes,faceBytes));
        ci.cancel();
    }
}
