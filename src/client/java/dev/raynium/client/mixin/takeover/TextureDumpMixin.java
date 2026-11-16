package dev.raynium.client.mixin.takeover;
import com.mojang.blaze3d.platform.TextureUtil;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
@Mixin(TextureUtil.class)
abstract class TextureDumpMixin {
    @Inject(method="writeAsPNG",at=@At("HEAD"),cancellable=true)
    private static void raynium$dump(java.nio.file.Path directory,String prefix,com.mojang.renderpearl.api.textures.GpuTexture texture,
            int maxMip,java.util.function.IntUnaryOperator modifier,CallbackInfo ci) {
        if(texture.getFormat()!=com.mojang.renderpearl.api.GpuFormat.RGBA8_UNORM || maxMip<0 || maxMip>=texture.getMipLevels())
            throw new IllegalArgumentException("Texture export requires RGBA8 and a valid mip range");
        long bytes=0;
        for(int mip=0;mip<=maxMip;mip++) bytes+=(long)texture.getWidth(mip)*texture.getHeight(mip)*4;
        if(bytes>Integer.MAX_VALUE) throw new IllegalArgumentException("Exporting textures larger than 2GB is not supported");
        var source=((dev.raynium.client.takeover.TakeoverHandles.Texture)texture).nativeTexture();
        for(int mip=0;mip<=maxMip;mip++) {
            final int level=mip;
            dev.raynium.client.takeover.TakeoverScreenshots.texture(source,mip,0,capture->{
                try(var image=new com.mojang.blaze3d.platform.NativeImage(capture.width(),capture.height(),false)) {
                    var pixels=java.nio.ByteBuffer.wrap(capture.rgba()).order(java.nio.ByteOrder.nativeOrder());
                    for(int y=0;y<capture.height();y++) for(int x=0;x<capture.width();x++)
                        image.setPixelABGR(x,y,modifier.applyAsInt(pixels.getInt((y*capture.width()+x)*4)));
                    java.nio.file.Files.createDirectories(directory);
                    image.writeToFile(directory.resolve(prefix+"_"+level+".png"));
                } catch(java.io.IOException failure) { org.slf4j.LoggerFactory.getLogger("raynium").warn("Cannot export texture {} mip {}",prefix,level,failure); }
            });
        }
        ci.cancel();
    }
}
