package dev.raynium.client.mixin;

import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.client.takeover.scene.ScenePrograms;
import dev.raynium.client.validation.VisualValidation;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(ScenePrograms.class)
abstract class VisualValidationNativePipelineMixin {
    @Inject(method="require",at=@At("HEAD"))
    private static void raynium$pipeline(RenderPipeline pipeline,CallbackInfoReturnable<ScenePrograms.Program> callback) {
        VisualValidation.pipeline(pipeline);
    }
}
