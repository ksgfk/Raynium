package dev.raynium.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.raynium.client.world.WorldModelCaptureScope;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(LevelRenderer.class)
abstract class LevelWorldFeatureScopeMixin {
    @WrapOperation(
        method = "render",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher;prepareFrame(Lnet/minecraft/client/renderer/SubmitNodeStorage;)Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher$PreparedFrame;"
        )
    )
    private FeatureRenderDispatcher.PreparedFrame raynium$scopeWorldModels(
            FeatureRenderDispatcher dispatcher, SubmitNodeStorage storage,
            Operation<FeatureRenderDispatcher.PreparedFrame> original) {
        WorldModelCaptureScope.enter();
        try {
            return original.call(dispatcher, storage);
        } finally {
            WorldModelCaptureScope.exit();
        }
    }
}
