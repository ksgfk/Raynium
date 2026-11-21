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
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

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
    @Shadow public boolean visible;
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

    @Inject(method = "render(Lcom/mojang/blaze3d/vertex/PoseStack;Lcom/mojang/blaze3d/vertex/VertexConsumer;III)V",
            at = @At("HEAD"), cancellable = true)
    private void raynium$fastLeafRender(PoseStack poseStack, VertexConsumer buffer,
            int lightCoords, int overlayCoords, int color, CallbackInfo ci) {
        if (!this.visible) {
            ci.cancel();
            return;
        }
        if (this.children.isEmpty()) {
            if (this.skipDraw || this.cubes.isEmpty()) {
                ci.cancel();
                return;
            }
            if (this.xRot == 0.0F && this.yRot == 0.0F && this.zRot == 0.0F
                    && this.xScale == 1.0F && this.yScale == 1.0F && this.zScale == 1.0F) {
                if (this.x == 0.0F && this.y == 0.0F && this.z == 0.0F) {
                    this.compile(poseStack.last(), buffer, lightCoords, overlayCoords, color);
                    ci.cancel();
                    return;
                }
                if (ModelFastTrack.tryCompileTranslatedLeaf((ModelPart) (Object) this, poseStack.last(), buffer,
                        this.x / 16.0F, this.y / 16.0F, this.z / 16.0F, lightCoords, overlayCoords, color)) {
                    ci.cancel();
                }
            }
        }
    }

    @Inject(method = "compile", at = @At("HEAD"), cancellable = true)
    private void raynium$fastCompile(PoseStack.Pose pose, VertexConsumer builder,
            int lightCoords, int overlayCoords, int color, CallbackInfo ci) {
        if (ModelFastTrack.tryCompilePart((ModelPart) (Object) this, pose, builder, lightCoords, overlayCoords, color)) {
            ci.cancel();
        }
    }
}
