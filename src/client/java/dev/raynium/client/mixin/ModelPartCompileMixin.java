package dev.raynium.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.raynium.client.world.ModelFastTrack;
import dev.raynium.client.world.RetainedModelManager;
import java.util.List;
import java.util.Map;
import net.minecraft.client.model.geom.ModelPart;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import com.llamalad7.mixinextras.injector.ModifyExpressionValue;
import com.llamalad7.mixinextras.injector.v2.WrapWithCondition;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(ModelPart.class)
abstract class ModelPartCompileMixin implements RetainedModelManager.PartHandle {
    @Shadow public float x;
    @Shadow public float y;
    @Shadow public float z;
    @Shadow public float xRot;
    @Shadow public float yRot;
    @Shadow public float zRot;
    @Shadow public float xScale;
    @Shadow public float yScale;
    @Shadow public float zScale;
    @Shadow public boolean skipDraw;
    @Shadow @Final private List<ModelPart.Cube> cubes;
    @Shadow @Final private Map<String, ModelPart> children;
    @Shadow private void compile(PoseStack.Pose pose, VertexConsumer builder, int lightCoords, int overlayCoords, int color) {
        throw new AssertionError();
    }

    @Unique private long raynium$meshEpoch;
    @Unique private int raynium$meshId;

    @Override public long raynium$getMeshEpoch() { return raynium$meshEpoch; }
    @Override public int raynium$getMeshId() { return raynium$meshId; }
    @Unique private int raynium$meshPage;
    @Override public int raynium$getMeshPage() { return raynium$meshPage; }
    @Override public void raynium$setMeshCache(long epoch, int meshId, int page) {
        this.raynium$meshPage = page;
        this.raynium$meshEpoch = epoch;
        this.raynium$meshId = meshId;
    }
    @Override public List<ModelPart.Cube> raynium$getCubes() { return cubes; }

    @ModifyExpressionValue(method = "render(Lcom/mojang/blaze3d/vertex/PoseStack;Lcom/mojang/blaze3d/vertex/VertexConsumer;III)V",
        at = @At(value = "FIELD", target = "Lnet/minecraft/client/model/geom/ModelPart;visible:Z"))
    private boolean raynium$renderAfterLeafFastPath(boolean visible, PoseStack poseStack, VertexConsumer buffer,
            int lightCoords, int overlayCoords, int color) {
        if (!visible || !children.isEmpty()) return visible;
        if (skipDraw || cubes.isEmpty()) return false;
        if (xRot != 0 || yRot != 0 || zRot != 0 || xScale != 1 || yScale != 1 || zScale != 1) return true;
        var pose = poseStack.last();
        var part = (ModelPart) (Object) this;
        if (x == 0 && y == 0 && z == 0) {
            if (!ModelFastTrack.tryCompilePart(part, pose, buffer, lightCoords, overlayCoords, color))
                compile(pose, buffer, lightCoords, overlayCoords, color);
            return false;
        }
        return !ModelFastTrack.tryCompileTranslatedLeaf(part, pose, buffer, x / 16, y / 16, z / 16,
            lightCoords, overlayCoords, color);
    }

    @WrapWithCondition(method = "render(Lcom/mojang/blaze3d/vertex/PoseStack;Lcom/mojang/blaze3d/vertex/VertexConsumer;III)V",
        at = @At(value = "INVOKE", target = "Lnet/minecraft/client/model/geom/ModelPart;compile(Lcom/mojang/blaze3d/vertex/PoseStack$Pose;Lcom/mojang/blaze3d/vertex/VertexConsumer;III)V"))
    private boolean raynium$needsCubeCompile(ModelPart part, PoseStack.Pose pose, VertexConsumer builder,
            int lightCoords, int overlayCoords, int color) {
        return !ModelFastTrack.tryCompilePart(part, pose, builder, lightCoords, overlayCoords, color);
    }
}
