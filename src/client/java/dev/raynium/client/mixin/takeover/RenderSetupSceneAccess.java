package dev.raynium.client.mixin.takeover;

import net.minecraft.client.renderer.rendertype.*;
import net.minecraft.client.renderer.oit.OitPipelineSet;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(RenderSetup.class)
public interface RenderSetupSceneAccess {
    @Accessor("layeringTransform") LayeringTransform raynium$layering();
    @Accessor("textureTransform") TextureTransform raynium$texturing();
    @Accessor("oitPipelineSet") OitPipelineSet raynium$oit();
}
