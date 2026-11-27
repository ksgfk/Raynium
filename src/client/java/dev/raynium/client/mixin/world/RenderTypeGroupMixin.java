package dev.raynium.client.mixin.world;

import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.raynium.client.world.ModelFastTrack;
import java.util.List;
import net.minecraft.client.renderer.StagedVertexBuffer;
import net.minecraft.client.renderer.feature.RenderTypeFeatureRenderer;
import net.minecraft.client.renderer.rendertype.PreparedRenderType;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(RenderTypeFeatureRenderer.Group.class)
abstract class RenderTypeGroupMixin implements ModelFastTrack.GroupAccess {
    @Shadow
    @Final
    private boolean canReorder;
    @Shadow
    @Final
    private List<StagedVertexBuffer.Draw> draws;
    @Shadow
    @Final
    private List<PreparedRenderType> drawRenderTypes;
    @Shadow
    public abstract VertexConsumer getVertexBuilder(RenderType renderType);

    @Unique
    private ModelFastTrack.@Nullable GroupExtension raynium$ext;
    @Unique
    private @Nullable RenderType raynium$lastPreparedKey;
    @Unique
    private @Nullable PreparedRenderType raynium$lastPreparedValue;

    @Inject(method = "getOrAddDraw", at = @At("RETURN"))
    private void raynium$trackFallbackDraw(RenderType renderType, CallbackInfoReturnable<StagedVertexBuffer.Draw> cir) {
        if (raynium$ext != null) {
            raynium$ext.syncFallbackDraws(this.draws.size());
        }
    }

    @Override
    public ModelFastTrack.GroupExtension raynium$extension() {
        if (raynium$ext == null) {
            raynium$ext = ModelFastTrack.acquireGroupExtension(this.canReorder, this.draws.size());
        }
        return raynium$ext;
    }

    @Override
    public ModelFastTrack.@Nullable GroupExtension raynium$extensionOrNull() {
        return raynium$ext;
    }

    @Override
    public void raynium$recycleExtension() {
        ModelFastTrack.GroupExtension ext = raynium$ext;
        if (ext != null) {
            raynium$ext = null;
            ext.recycle();
        }
    }

    @Override
    public PreparedRenderType raynium$prepareRenderType(RenderType renderType) {
        if (raynium$lastPreparedKey == renderType && raynium$lastPreparedValue != null) {
            return raynium$lastPreparedValue;
        }
        PreparedRenderType prepared = renderType.prepare();
        raynium$lastPreparedKey = renderType;
        raynium$lastPreparedValue = prepared;
        return prepared;
    }

    @Override
    public VertexConsumer raynium$fallbackConsumer(RenderType renderType) {
        return this.getVertexBuilder(renderType);
    }

    @Override
    public List<StagedVertexBuffer.Draw> raynium$draws() {
        return this.draws;
    }

    @Override
    public List<PreparedRenderType> raynium$drawRenderTypes() {
        return this.drawRenderTypes;
    }
}
