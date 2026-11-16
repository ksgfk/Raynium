package dev.raynium.client.mixin.takeover;

import java.util.List;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.RenderTypeFeatureRenderer;
import net.minecraft.client.renderer.feature.submit.SubmitNode;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

@Mixin(RenderTypeFeatureRenderer.class)
public interface FeatureBuildAccess {
    @Invoker("buildGroup") void raynium$build(FeatureFrameContext context,List<SubmitNode> submits);
}
