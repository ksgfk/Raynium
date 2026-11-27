package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.DefaultVertexFormat;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.world.ParticleFastTrack;
import dev.raynium.client.world.ParticleFastTrack.PreparedLayers;
import it.unimi.dsi.fastutil.objects.Reference2IntOpenHashMap;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.StagedVertexBuffer;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.QuadParticleFeatureRenderer;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.state.level.QuadParticleRenderState;
import net.minecraft.client.renderer.texture.AbstractTexture;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(QuadParticleFeatureRenderer.class)
abstract class QuadParticleFeatureMixin {
    @Shadow @Final private List<QuadParticleFeatureRenderer.PreparedGroup> groups;
    @Unique private @Nullable ArrayList<@Nullable PreparedLayers> raynium$compactGroups;

    @Inject(method = "prepareGroup", at = @At("HEAD"), cancellable = true)
    private void raynium$prepareCompactParticles(FeatureFrameContext context, List<QuadParticleFeatureRenderer.Submit> submits,
            boolean strictlyOrdered, CallbackInfo ci) {
        if (submits.isEmpty()) return;
        if (!(RenderSystem.getDevice() instanceof RayniumGpuDevice device)) return;
        boolean useOit = Minecraft.getInstance().gameRenderer.useImprovedTransparency();
        // Vanilla has three atlases per transparency mode. A negative count vetoes the whole layer.
        var slices = new Reference2IntOpenHashMap<SingleQuadParticle.Layer>(3);
        for (QuadParticleFeatureRenderer.Submit submit : submits) {
            QuadParticleRenderState particles = submit.particles();
            if (particles.isEmpty()) continue;
            boolean exactClass = particles.getClass() == QuadParticleRenderState.class;
            for (SingleQuadParticle.Layer layer : particles.layers()) {
                if (layer.translucent() != submit.translucent()) continue;
                int previous = slices.getInt(layer);
                if (previous < 0) continue;
                if (!exactClass || !ParticleFastTrack.isLayerEligible(device, layer, useOit)) {
                    slices.put(layer, -1);
                } else {
                    var raw = (ParticleFastTrack.RawParticleAccess) (Object) particles;
                    slices.put(layer, raw.raynium$hasCompactSlice(layer) ? previous + 1 : previous);
                }
            }
        }
        int compactLayers = 0;
        for (var counts = slices.values().iterator(); counts.hasNext();) {
            if (counts.nextInt() >= 0) ++compactLayers;
        }
        if (compactLayers == 0) return;

        StagedVertexBuffer stagedVertexBuffer = context.stagedVertexBuffer();
        int groupIndex = groups.size();
        if (raynium$compactGroups == null) raynium$compactGroups = new ArrayList<>(groupIndex + 1);
        else if (raynium$compactGroups.size() < groupIndex) raynium$compactGroups.ensureCapacity(groupIndex + 1);
        while (raynium$compactGroups.size() < groupIndex) raynium$compactGroups.add(null);
        var prepared = new PreparedLayers(slices.size() - compactLayers, compactLayers);
        raynium$compactGroups.add(prepared);

        for (QuadParticleFeatureRenderer.Submit submit : submits) {
            QuadParticleRenderState particles = submit.particles();
            if (particles.isEmpty()) continue;
            for (SingleQuadParticle.Layer layer : particles.layers()) {
                if (layer.translucent() != submit.translucent()) continue;
                AbstractTexture texture = context.textureManager().getTexture(layer.textureAtlasLocation());
                int sliceCount = slices.getOrDefault(layer, -1);
                if (sliceCount >= 0) {
                    var batch = prepared.compact.get(layer);
                    if (batch == null) {
                        batch = new ParticleFastTrack.CompactLayerBatch(layer, sliceCount);
                        prepared.compact.put(layer, batch);
                    }
                    batch.setTexture(texture);
                    ((ParticleFastTrack.RawParticleAccess) (Object) particles).raynium$appendCompactLayer(layer, batch);
                } else {
                    StagedVertexBuffer.Draw draw = prepared.draws.get(layer);
                    if (draw == null) {
                        draw = stagedVertexBuffer.appendDraw(DefaultVertexFormat.PARTICLE, PrimitiveTopology.QUADS, null);
                        prepared.draws.put(layer, draw);
                    }
                    particles.buildLayer(layer, stagedVertexBuffer.getVertexBuilder(draw));
                    prepared.textures.put(layer, texture);
                    stagedVertexBuffer.requestIndexCount(draw);
                }
            }
        }
        boolean translucent = submits.getFirst().translucent();
        this.groups.add(new QuadParticleFeatureRenderer.PreparedGroup(prepared.draws, prepared.textures, translucent));
        ci.cancel();
    }

    @Inject(
        method = "executeGroup",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/feature/QuadParticleFeatureRenderer;drawLayers(Lnet/minecraft/client/renderer/StagedVertexBuffer;Lnet/minecraft/client/renderer/feature/QuadParticleFeatureRenderer$PreparedGroup;Lcom/mojang/renderpearl/api/commands/RenderPass;Lnet/minecraft/client/renderer/oit/OitStage;)V"
        )
    )
    private void raynium$executeCompactParticles(FeatureFrameContext context, @Nullable OitStage stage,
            RenderPass renderPass, int groupIndex, List<QuadParticleFeatureRenderer.Submit> submits,
            boolean strictlyOrdered, CallbackInfo ci) {
        if (raynium$compactGroups == null || groupIndex >= raynium$compactGroups.size()) return;
        var group = raynium$compactGroups.get(groupIndex);
        if (group == null || group.compact.isEmpty()) return;
        for (ParticleFastTrack.CompactLayerBatch batch : group.compact.values()) {
            batch.execute(renderPass, stage);
        }
    }

    @Inject(method = "finishExecute", at = @At("TAIL"))
    private void raynium$clearCompactGroups(FeatureFrameContext context, CallbackInfo ci) {
        raynium$compactGroups = null;
    }
}
