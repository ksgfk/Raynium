package dev.raynium.client.mixin;
import dev.raynium.client.terrain.TerrainCapture;
import dev.raynium.client.preview.PreviewHost;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class GameTerrainMixin {
    @ModifyArg(method="renderLevel",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/ProjectionMatrixBuffer;getBuffer(Lorg/joml/Matrix4f;)Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;"),index=0)
    private Matrix4f raynium$projection(Matrix4f matrix) { TerrainCapture.projection(matrix); return matrix; }
    @Inject(method="render",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/Lightmap;render(Lnet/minecraft/client/renderer/state/LightmapRenderState;)V",shift=At.Shift.AFTER))
    private void raynium$inputs(CallbackInfo ci) { ((PreviewHost)Minecraft.getInstance()).raynium$prepareTerrain(); }
}
