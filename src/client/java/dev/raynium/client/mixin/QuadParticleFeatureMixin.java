package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.DefaultVertexFormat;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.world.ParticleFastTrack;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
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
    @Unique private final List<@Nullable Map<SingleQuadParticle.Layer, ParticleFastTrack.CompactLayerBatch>> raynium$compactGroups = new ArrayList<>();

    @Inject(method = "prepareGroup", at = @At("HEAD"), cancellable = true)
    private void raynium$prepareCompactParticles(FeatureFrameContext context, List<QuadParticleFeatureRenderer.Submit> submits,
            boolean strictlyOrdered, CallbackInfo ci) {
        if (submits.isEmpty()) return;
        if (!(RenderSystem.getDevice() instanceof RayniumGpuDevice device)) {
            raynium$compactGroups.add(null);
            return;
        }
        boolean useOit = Minecraft.getInstance().gameRenderer.useImprovedTransparency();
        Map<SingleQuadParticle.Layer, Boolean> eligible = new IdentityHashMap<>();
        boolean anyEligible = false;
        for (QuadParticleFeatureRenderer.Submit submit : submits) {
            QuadParticleRenderState particles = submit.particles();
            if (particles.isEmpty()) continue;
            boolean exactClass = particles.getClass() == QuadParticleRenderState.class;
            for (SingleQuadParticle.Layer layer : particles.layers()) {
                if (layer.translucent() != submit.translucent()) continue;
                boolean layerOk = exactClass && ParticleFastTrack.isLayerEligible(device, layer, useOit);
                eligible.merge(layer, layerOk, (a, b) -> a && b);
            }
        }
        for (Boolean ok : eligible.values()) {
            if (Boolean.TRUE.equals(ok)) { anyEligible = true; break; }
        }
        if (!anyEligible) {
            raynium$compactGroups.add(null);
            return;
        }

        StagedVertexBuffer stagedVertexBuffer = context.stagedVertexBuffer();
        Map<SingleQuadParticle.Layer, StagedVertexBuffer.Draw> drawByLayer = new IdentityHashMap<>();
        Map<SingleQuadParticle.Layer, AbstractTexture> textures = new IdentityHashMap<>();
        Map<SingleQuadParticle.Layer, ParticleFastTrack.CompactLayerBatch> compactByLayer = new IdentityHashMap<>();

        for (QuadParticleFeatureRenderer.Submit submit : submits) {
            QuadParticleRenderState particles = submit.particles();
            if (particles.isEmpty()) continue;
            for (SingleQuadParticle.Layer layer : particles.layers()) {
                if (layer.translucent() != submit.translucent()) continue;
                AbstractTexture texture = context.textureManager().getTexture(layer.textureAtlasLocation());
                if (Boolean.TRUE.equals(eligible.get(layer))) {
                    ParticleFastTrack.CompactLayerBatch batch = compactByLayer.computeIfAbsent(layer, ParticleFastTrack::acquireBatch);
                    batch.setTexture(texture);
                    ((ParticleFastTrack.RawParticleAccess) (Object) particles).raynium$appendCompactLayer(layer, batch);
                } else {
                    StagedVertexBuffer.Draw draw = drawByLayer.computeIfAbsent(
                        layer, l -> stagedVertexBuffer.appendDraw(DefaultVertexFormat.PARTICLE, PrimitiveTopology.QUADS, null)
                    );
                    particles.buildLayer(layer, stagedVertexBuffer.getVertexBuilder(draw));
                    textures.put(layer, texture);
                    stagedVertexBuffer.requestIndexCount(draw);
                }
            }
        }
        boolean translucent = submits.getFirst().translucent();
        this.groups.add(new QuadParticleFeatureRenderer.PreparedGroup(drawByLayer, textures, translucent));
        raynium$compactGroups.add(compactByLayer);
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
        if (groupIndex >= raynium$compactGroups.size()) return;
        Map<SingleQuadParticle.Layer, ParticleFastTrack.CompactLayerBatch> compact = raynium$compactGroups.get(groupIndex);
        if (compact == null || compact.isEmpty()) return;
        for (ParticleFastTrack.CompactLayerBatch batch : compact.values()) {
            batch.execute(renderPass, stage);
        }
    }

    @Inject(method = "finishExecute", at = @At("TAIL"))
    private void raynium$clearCompactGroups(FeatureFrameContext context, CallbackInfo ci) {
        for (int i = 0; i < raynium$compactGroups.size(); ++i) {
            Map<SingleQuadParticle.Layer, ParticleFastTrack.CompactLayerBatch> compact = raynium$compactGroups.get(i);
            if (compact != null) {
                for (ParticleFastTrack.CompactLayerBatch batch : compact.values()) {
                    batch.recycle();
                }
            }
        }
        raynium$compactGroups.clear();
    }
}
