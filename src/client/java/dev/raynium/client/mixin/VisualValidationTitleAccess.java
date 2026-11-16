package dev.raynium.client.mixin;

import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.client.gui.components.SplashRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(TitleScreen.class)
public interface VisualValidationTitleAccess {
    @Accessor("splash") void raynium$splash(SplashRenderer value);
}
