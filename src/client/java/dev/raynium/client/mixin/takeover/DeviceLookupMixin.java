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
@Mixin(targets={"net.minecraft.client.gui.components.debug.DebugEntrySystemSpecs","net.minecraft.client.Options","net.minecraft.client.renderer.ProjectionMatrixBuffer","net.minecraft.client.renderer.MappableRingBuffer","com.mojang.blaze3d.platform.Lighting","net.minecraft.client.renderer.GlobalSettingsUniform","net.minecraft.client.renderer.WorldBorderRenderer","net.minecraft.client.renderer.fog.FogRenderer","net.minecraft.client.renderer.CubeMap","net.minecraft.client.renderer.DebugCrosshairRenderer","net.minecraft.client.GraphicsPreset","net.minecraft.client.renderer.texture.TextureAtlas","net.minecraft.client.renderer.LevelRenderer","net.minecraft.client.renderer.Lightmap","net.minecraft.client.renderer.texture.DynamicTexture","com.mojang.blaze3d.systems.SamplerCache","net.minecraft.client.renderer.ShaderManager","net.minecraft.client.gui.font.FontTexture","net.minecraft.client.gui.font.glyphs.SpecialGlyphs$1","net.minecraft.client.renderer.texture.ReloadableTexture","net.minecraft.client.renderer.texture.MipmappedTexture","net.minecraft.client.gui.font.providers.UnihexProvider$Glyph$2","com.mojang.blaze3d.font.TrueTypeGlyphProvider$Glyph$1","net.minecraft.client.renderer.GpuWarnlistManager$Preparations","net.minecraft.client.resources.palette.PalettedTextureManager","net.minecraft.client.resources.palette.PalettedTextureManager$AtlasTexture","net.minecraft.client.Camera","net.minecraft.client.renderer.Projection","net.minecraft.client.renderer.chunk.SectionRenderDispatcher"})
abstract class DeviceLookupMixin {
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;getDevice()Lcom/mojang/renderpearl/api/device/GpuDevice;"))
    private static GpuDevice raynium$device(Operation<GpuDevice> original) { return null; }
}
