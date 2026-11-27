package dev.raynium.client.mixin.world;

import com.mojang.renderpearl.api.commands.RenderPass;
import dev.raynium.client.world.ModelFastTrack;
import java.util.List;
import java.util.Objects;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.RenderTypeFeatureRenderer;
import net.minecraft.client.renderer.feature.submit.SubmitNode;
import net.minecraft.client.renderer.oit.OitStage;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(RenderTypeFeatureRenderer.class)
abstract class RenderTypeFeatureRendererMixin<Submit extends SubmitNode> implements ModelFastTrack.FeatureRendererAccess {
    @Shadow
    private RenderTypeFeatureRenderer.@Nullable Group currentGroup;
    @Shadow
    @Final
    private List<RenderTypeFeatureRenderer.Group> groups;

    @Override
    public ModelFastTrack.GroupAccess raynium$currentGroupAccess() {
        return (ModelFastTrack.GroupAccess)(Object)Objects.requireNonNull(this.currentGroup, "Not preparing group");
    }

    @Inject(method = "executeGroup", at = @At("HEAD"), cancellable = true)
    private void raynium$executeFastModels(FeatureFrameContext context, @Nullable OitStage stage,
                                           RenderPass renderPass, int groupIndex, List<Submit> submits, boolean strictlyOrdered, CallbackInfo ci) {
        ModelFastTrack.GroupAccess group = (ModelFastTrack.GroupAccess)(Object)this.groups.get(groupIndex);
        ModelFastTrack.GroupExtension ext = group.raynium$extensionOrNull();
        if (ext == null)
            return;
        ext.execute(context.stagedVertexBuffer(), stage, renderPass, group.raynium$draws(), group.raynium$drawRenderTypes());
        ci.cancel();
    }

    @Inject(method = "finishExecute", at = @At("HEAD"))
    private void raynium$recycleFastModels(FeatureFrameContext context, CallbackInfo ci) {
        for (int i = 0; i < this.groups.size(); ++i) {
            ((ModelFastTrack.GroupAccess)(Object)this.groups.get(i)).raynium$recycleExtension();
        }
        ModelFastTrack.finishGrouping();
    }
}
