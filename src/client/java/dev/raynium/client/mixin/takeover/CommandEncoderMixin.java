package dev.raynium.client.mixin.takeover;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.device.*;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import dev.raynium.client.takeover.*;
import java.nio.ByteBuffer;
import java.util.OptionalDouble;
import java.util.function.Supplier;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
@Mixin(targets={"net.minecraft.client.renderer.Lightmap","net.minecraft.client.renderer.texture.DynamicTexture","com.mojang.blaze3d.platform.Lighting","net.minecraft.client.renderer.ProjectionMatrixBuffer","net.minecraft.client.renderer.GlobalSettingsUniform","net.minecraft.client.gui.font.glyphs.SpecialGlyphs$1","net.minecraft.client.renderer.texture.ReloadableTexture","net.minecraft.client.renderer.texture.MipmappedTexture","net.minecraft.client.gui.font.providers.UnihexProvider$Glyph$2","com.mojang.blaze3d.font.TrueTypeGlyphProvider$Glyph$1","net.minecraft.client.resources.palette.PalettedTextureManager","net.minecraft.client.renderer.chunk.SectionRenderDispatcher"})
abstract class CommandEncoderMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createCommandEncoder()Lcom/mojang/renderpearl/api/commands/CommandEncoder;"))
    private static CommandEncoder raynium$encoder(GpuDevice device,Operation<CommandEncoder> original) { return null; }
}
