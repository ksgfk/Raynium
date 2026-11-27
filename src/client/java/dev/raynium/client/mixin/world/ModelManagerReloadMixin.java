package dev.raynium.client.mixin.world;

import dev.raynium.client.world.RetainedModelManager;
import net.minecraft.client.resources.model.ModelManager;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ModelManager.class)
abstract class ModelManagerReloadMixin {
    @Inject(method = "apply", at = @At("TAIL"))
    private void raynium$invalidateRetainedModels(ModelManager.ReloadState preparations, CallbackInfo ci) {
        RetainedModelManager.invalidateAll();
    }
}
