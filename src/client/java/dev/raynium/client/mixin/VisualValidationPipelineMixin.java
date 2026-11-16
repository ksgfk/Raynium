package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.pipeline.CompiledRenderPipeline;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.client.validation.VisualValidation;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(RenderSystem.class)
abstract class VisualValidationPipelineMixin {
    @Inject(method="getCompiledPipelineNullable",at=@At("HEAD"))
    private static void raynium$pipeline(RenderPipeline pipeline,CallbackInfoReturnable<CompiledRenderPipeline> callback) {
        VisualValidation.pipeline(pipeline);
    }
}
