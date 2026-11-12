package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import dev.raynium.client.takeover.TakeoverAtlas;
import dev.raynium.client.terrain.TerrainCapture;
import java.util.List;
import net.minecraft.client.renderer.texture.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import com.mojang.renderpearl.api.device.GpuDevice;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.client.takeover.TakeoverHandles;
import net.minecraft.client.renderer.texture.TextureAtlas;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(TextureAtlas.class)
abstract class AtlasTakeoverMixin {
    @Shadow @Final private net.minecraft.resources.Identifier location;
    @Shadow private List<TextureAtlasSprite> sprites;
    @Unique private TakeoverAtlas raynium$atlas;
    @WrapOperation(method="upload",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/texture/TextureAtlasSprite;isAnimated()Z"))
    private boolean raynium$noGpuAnimation(TextureAtlasSprite sprite,Operation<Boolean> original) { return false; }
    @Inject(method="uploadInitialContents",at=@At("HEAD"),cancellable=true)
    private void raynium$initial(CallbackInfo ci) { raynium$atlas=new TakeoverAtlas((TextureAtlas)(Object)this,sprites); ci.cancel(); }
    @Inject(method="cycleAnimationFrames",at=@At("HEAD"),cancellable=true)
    private void raynium$animate(CallbackInfo ci) { if(raynium$atlas!=null) raynium$atlas.tick(); ci.cancel(); }
    @Inject(method="clearTextureData",at=@At("HEAD"))
    private void raynium$clear(CallbackInfo ci) { raynium$atlas=null; }
    @Inject(method="upload",at=@At("RETURN"))
    private void raynium$invalidate(CallbackInfo ci) { if(location.equals(TextureAtlas.LOCATION_BLOCKS)) TerrainCapture.invalidate(); }
    @WrapOperation(method="createTexture",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createTextureView(Lcom/mojang/renderpearl/api/textures/GpuTexture;II)Lcom/mojang/renderpearl/api/textures/GpuTextureView;"))
    private GpuTextureView raynium$view(GpuDevice device,GpuTexture texture,int base,int count,Operation<GpuTextureView> original) { return TakeoverHandles.view(texture,base,count); }
}
