package dev.raynium.client.mixin;

import net.minecraft.client.renderer.Panorama;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(Panorama.class)
public interface VisualValidationPanoramaAccess {
    @Accessor("spin") void raynium$spin(float value);
}
