package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.world.ModelFastTrack;
import dev.raynium.client.world.ShadowFastTrack;
import dev.raynium.client.world.WorldModelCaptureScope;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.ShadowFeatureRenderer;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ShadowFeatureRenderer.class)
abstract class ShadowFeatureRendererMixin {
    @Shadow @Final private static RenderType SHADOW_RENDER_TYPE;

    @Inject(method = "buildGroup", at = @At("HEAD"), cancellable = true)
    private void raynium$prepareShadows(FeatureFrameContext context, List<ShadowFeatureRenderer.Submit> submits, CallbackInfo ci) {
        if (!WorldModelCaptureScope.isActive() || !(RenderSystem.getDevice() instanceof RayniumGpuDevice device)
            || !device.supportsInstanced(SHADOW_RENDER_TYPE.pipeline(), "shadow")) return;
        var group = ((ModelFastTrack.FeatureRendererAccess) this).raynium$currentGroupAccess();
        var prepared = group.raynium$prepareRenderType(SHADOW_RENDER_TYPE);
        var oit = prepared.oitPipelineSet();
        if (Minecraft.getInstance().gameRenderer.useImprovedTransparency() && oit != null &&
            !(device.supportsInstanced(oit.getPipeline(OitStage.DEPTH_BOUNDS), "shadow") &&
              device.supportsInstanced(oit.getPipeline(OitStage.TRANSMITTANCE), "shadow") &&
              device.supportsInstanced(oit.getPipeline(OitStage.ACCUMULATE), "shadow"))) return;
        group.raynium$extension().addShadow(new ShadowFastTrack(prepared, submits, device.currentBatch()));
        ci.cancel();
    }
}
