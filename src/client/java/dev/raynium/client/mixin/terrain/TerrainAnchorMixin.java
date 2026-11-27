package dev.raynium.client.mixin.terrain;

import com.llamalad7.mixinextras.injector.wrapmethod.WrapMethod;
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.textures.FilterMode;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.terrain.RetainedTerrain;
import dev.raynium.diagnostics.RayniumTelemetry;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.ChunkSectionsToRender;
import net.minecraft.client.renderer.oit.OitRenderPassProvider;
import net.minecraft.client.renderer.oit.OitStage;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
@Mixin(ChunkSectionsToRender.class)
abstract class TerrainAnchorMixin implements RetainedTerrain.ViewOwner {
    @Unique
    private CommandTape.TerrainView raynium$view;
    @Override
    public void raynium$setView(CommandTape.TerrainView view) {
        raynium$view = view;
    }
    @WrapOperation(method = {"renderGroup", "renderOit"}, at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/chunk/ChunkSectionsToRender;renderLayers([Lnet/minecraft/client/renderer/chunk/ChunkSectionLayer;Lcom/mojang/renderpearl/api/textures/GpuSampler;Lcom/mojang/renderpearl/api/commands/RenderPass;Lcom/mojang/renderpearl/api/textures/GpuTextureView;Lcom/mojang/renderpearl/api/textures/GpuTextureView;Lcom/mojang/renderpearl/api/pipeline/RenderPipeline;Lcom/mojang/renderpearl/api/pipeline/RenderPipeline;)V"))
    private void raynium$layers(ChunkSectionsToRender sections, ChunkSectionLayer[] layers, GpuSampler sampler, RenderPass pass,
                                GpuTextureView atlas, GpuTextureView light, RenderPipeline override, RenderPipeline multi, Operation<Void> original) {
        boolean indirect = sections instanceof ChunkSectionsToRender.DrawIndirect;
        var atlasBinding = new TextureViewAndSampler(atlas, sampler);
        var linear = RenderSystem.getSamplerCache().getClampToEdge(FilterMode.LINEAR);
        var lightBinding = new TextureViewAndSampler(light, linear);
        for (var layer : layers) {
            var pipeline = indirect ? multi : override;
            RayniumTelemetry.onPipelineSelected(pipeline == null ? layer.pipeline(indirect) : pipeline);
            if (raynium$oitStage == null)
                RayniumGpuDevice.recordTerrainStage(pass, raynium$view, layer.ordinal(), atlasBinding, lightBinding, null, null, null);
            else {
                var params = raynium$oitParams;
                boolean bounds = raynium$oitStage != OitStage.DEPTH_BOUNDS;
                boolean accumulate = raynium$oitStage == OitStage.ACCUMULATE;
                RayniumGpuDevice.recordTerrainStage(pass, raynium$view, 3 + raynium$oitStage.ordinal(), atlasBinding, lightBinding,
                                                    bounds ? params.depthBoundsTargetView : null, accumulate ? params.transmittanceTargetViews[0] : null,
                                                    accumulate ? params.transmittanceTargetViews[1] : null);
            }
        }
    }
    @Unique
    private OitStage raynium$oitStage;
    @Unique
    private OitRenderPassProvider.Parameters raynium$oitParams;
    @WrapMethod(method = "renderOit")
    private void raynium$oit(GpuSampler sampler, OitStage stage, OitRenderPassProvider.Parameters params, GpuTextureView atlas, GpuTextureView light, Operation<Void> original) {
        raynium$oitStage = stage;
        raynium$oitParams = params;
        try {
            original.call(sampler, stage, params, atlas, light);
        } finally {
            raynium$oitStage = null;
            raynium$oitParams = null;
        }
    }
}
