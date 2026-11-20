package dev.raynium.client.mixin;

import dev.raynium.client.terrain.TerrainCapture;
import net.minecraft.client.renderer.GameRenderer;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;

@Mixin(GameRenderer.class)
abstract class TerrainProjectionMixin {
    @ModifyArg(method="renderLevel",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/ProjectionMatrixBuffer;getBuffer(Lorg/joml/Matrix4f;)Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;"),index=0)
    private Matrix4f raynium$captureProjection(Matrix4f projection) {
        TerrainCapture.projection(projection);
        return projection;
    }
}
