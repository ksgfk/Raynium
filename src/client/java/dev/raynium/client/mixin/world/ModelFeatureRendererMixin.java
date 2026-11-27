package dev.raynium.client.mixin.world;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.DefaultVertexFormat;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.world.ModelFastTrack;
import dev.raynium.client.world.WorldModelCaptureScope;
import net.minecraft.client.Minecraft;
import net.minecraft.client.model.Model;
import net.minecraft.client.renderer.feature.ModelFeatureRenderer;
import net.minecraft.client.renderer.rendertype.PreparedRenderType;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ModelFeatureRenderer.class)
abstract class ModelFeatureRendererMixin {
    @Shadow
    @Final
    private PoseStack poseStack;
    @Unique
    private final ModelFastTrack.ModelSink raynium$sink = new ModelFastTrack.ModelSink();

    @Inject(method = "prepareModel", at = @At("HEAD"), cancellable = true)
    private <S> void raynium$fastPrepareModel(ModelFeatureRenderer.Submit<S> submit, CallbackInfo ci) {
        if (!WorldModelCaptureScope.isActive())
            return;
        if (!(RenderSystem.getDevice() instanceof RayniumGpuDevice device))
            return;
        if (submit.sheetedDecalPose() != null || submit.uvMapping() != null)
            return;
        RenderType renderType = submit.renderType();
        if (renderType.format() != DefaultVertexFormat.ENTITY || renderType.primitiveTopology() != PrimitiveTopology.QUADS)
            return;
        boolean useOit = Minecraft.getInstance().gameRenderer.useImprovedTransparency();
        if (renderType.sortOnUpload() && !useOit)
            return;
        if (!device.supportsInstanced(renderType.pipeline(), "entity"))
            return;

        ModelFastTrack.GroupAccess group = ((ModelFastTrack.FeatureRendererAccess)this).raynium$currentGroupAccess();
        PreparedRenderType prepared = group.raynium$prepareRenderType(renderType);
        if (useOit && prepared.oitPipelineSet() != null && !ModelFastTrack.supportsOitInstanced(device, prepared.oitPipelineSet())) {
            return;
        }

        this.poseStack.last().set(submit.pose());
        raynium$sink.begin(device, group, renderType, prepared);
        ModelFastTrack.beginCapture(raynium$sink);
        try {
            Model<? super S> model = submit.model();
            model.setupAnim(submit.state());
            model.renderToBuffer(this.poseStack, raynium$sink, submit.lightCoords(), submit.overlayCoords(), submit.tintedColor());
        } finally {
            ModelFastTrack.endCapture();
            raynium$sink.end();
        }
        ci.cancel();
    }
}
