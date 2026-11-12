package dev.raynium.client.mixin.takeover;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import java.util.List;
import net.minecraft.client.renderer.ShaderManager;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
@Mixin(ShaderManager.class)
abstract class ShaderReloadMixin {
    @WrapOperation(method="*",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/RenderPipelines;requiredPipelines()Ljava/util/List;"))
    private static List<RenderPipeline> raynium$required(Operation<List<RenderPipeline>> original) { return List.of(); }
    @WrapOperation(method="*",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/RenderPipelines;optionalPipelines()Ljava/util/List;"))
    private static List<RenderPipeline> raynium$optional(Operation<List<RenderPipeline>> original) { return List.of(); }
}
