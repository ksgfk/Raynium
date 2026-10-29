package dev.raynium.client.mixin;
import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.Lightmap;
import net.minecraft.client.renderer.state.LightmapRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Lightmap.class)
abstract class TerrainLightmapMixin {
    @ModifyArg(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createTexture(Ljava/lang/String;ILcom/mojang/renderpearl/api/GpuFormat;IIII)Lcom/mojang/renderpearl/api/textures/GpuTexture;"),index=1)
    private int raynium$copySource(int usage) { return TerrainCapture.ENABLED ? usage|2 : usage; }
    @Inject(method="render",at=@At("RETURN"))
    private void raynium$dirty(LightmapRenderState state,CallbackInfo ci) { if(state.needsUpdate) TerrainTextures.light(); }
}
