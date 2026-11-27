package dev.raynium.harness.mixin.validation;

import net.minecraft.client.gui.components.SplashRenderer;
import net.minecraft.client.gui.screens.TitleScreen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(TitleScreen.class)
public interface VisualValidationTitleAccessor {
    @Accessor("splash")
    void rayniumHarness$splash(SplashRenderer value);
}
