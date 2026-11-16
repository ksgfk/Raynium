package dev.raynium.client.mixin;

import dev.raynium.client.validation.VisualValidation;
import net.minecraft.client.gui.render.pip.PictureInPictureRenderer;
import net.minecraft.client.renderer.state.gui.GuiRenderState;
import net.minecraft.client.renderer.state.gui.pip.PictureInPictureRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(PictureInPictureRenderer.class)
abstract class VisualValidationPictureInPictureMixin {
    @Inject(method="blitTexture",at=@At("HEAD"))
    private void raynium$pictureInPicture(PictureInPictureRenderState state,GuiRenderState gui,CallbackInfo callback) {
        VisualValidation.pictureInPicture(state);
    }
}
