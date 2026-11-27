package dev.raynium.client.mixin.world;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.raynium.client.world.RetainedModelManager;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;

@Mixin(PoseStack.Pose.class)
abstract class PoseStackPoseMixin implements RetainedModelManager.PoseAccess {
    @Shadow
    private boolean trustedNormals;

    @Override
    public boolean raynium$trustedNormals() {
        return this.trustedNormals;
    }
}
