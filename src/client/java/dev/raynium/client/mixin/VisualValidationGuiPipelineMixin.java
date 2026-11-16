package dev.raynium.client.mixin;

import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.client.takeover.TakeoverGui;
import dev.raynium.client.validation.VisualValidation;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(TakeoverGui.class)
abstract class VisualValidationGuiPipelineMixin {
    @Inject(method="pipelineKind",at=@At("HEAD"))
    private static void raynium$pipeline(RenderPipeline pipeline,CallbackInfoReturnable<Integer> callback) {
        VisualValidation.pipeline(pipeline);
    }
}
