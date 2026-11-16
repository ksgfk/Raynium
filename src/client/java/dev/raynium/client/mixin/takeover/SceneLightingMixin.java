package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.platform.Lighting;
import dev.raynium.client.takeover.scene.SceneOffscreen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Lighting.class)
abstract class SceneLightingMixin {
    @Inject(method="setupFor",at=@At("HEAD"))
    private void raynium$lighting(Lighting.Entry entry,CallbackInfo ci) { SceneOffscreen.lighting(entry); }
}
