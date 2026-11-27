package dev.raynium.harness.mixin.validation;

import net.minecraft.client.renderer.Panorama;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(Panorama.class)
public interface VisualValidationPanoramaAccessor {
    @Accessor("spin")
    void rayniumHarness$spin(float value);
}
