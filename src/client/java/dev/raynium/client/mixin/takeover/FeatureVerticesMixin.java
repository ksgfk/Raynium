package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.raynium.client.takeover.scene.SceneFeatures;
import net.minecraft.client.renderer.feature.RenderTypeFeatureRenderer;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(RenderTypeFeatureRenderer.class)
abstract class FeatureVerticesMixin {
    @Inject(method="getVertexBuilder",at=@At("HEAD"),cancellable=true)
    private void raynium$vertices(RenderType type,CallbackInfoReturnable<VertexConsumer> ci) {
        var capture=SceneFeatures.active();
        if(capture!=null) ci.setReturnValue(capture.vertices(type));
    }
}
