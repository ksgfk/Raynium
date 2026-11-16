package dev.raynium.client.takeover.scene;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.*;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import com.mojang.renderpearl.api.textures.FilterMode;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.mixin.takeover.FeatureBuildAccess;
import dev.raynium.client.mixin.takeover.RenderTypeSceneAccess;
import dev.raynium.client.mixin.takeover.RenderSetupSceneAccess;
import java.lang.foreign.MemorySegment;
import java.util.*;
import net.minecraft.client.Minecraft;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.*;
import net.minecraft.client.renderer.feature.phase.FeatureRenderPhase;
import net.minecraft.client.renderer.feature.submit.SubmitNode;
import net.minecraft.client.renderer.rendertype.*;
import net.minecraft.client.renderer.oit.OitStage;
import org.joml.Matrix4f;

/** Reuses vanilla CPU feature builders and phase sorting, retaining MeshData until synchronous consumption. */
public final class SceneFeatures implements AutoCloseable,SceneInput {
    private static final OitStage[] OIT_STAGES=OitStage.values();
    private static SceneFeatures active;
    public static SceneFeatures active() { return active; }
    private static final class Batch implements AutoCloseable {
        final ByteBufferBuilder vertices=new ByteBufferBuilder(32768),indices=new ByteBufferBuilder(8192);
        BufferBuilder builder;
        MeshData mesh;
        RenderType type;
        int vertexBlob=-1,indexBlob=-1;
        void release() { if(mesh!=null) { mesh.close(); mesh=null; } vertices.clear(); indices.clear(); builder=null; type=null; vertexBlob=indexBlob=-1; }
        @Override public void close() { release(); vertices.close(); indices.close(); }
    }
    private final ArrayList<Batch> batches=new ArrayList<>();
    private final IdentityHashMap<RenderType,Batch> reorderable=new IdentityHashMap<>();
    private final ArrayList<SubmitNode> group=new ArrayList<>();
    private final IdentityHashMap<SingleQuadParticle.Layer,Batch> particleBatches=new IdentityHashMap<>();
    private int used,groupStart,phase,width,height,phaseOverride;
    private boolean strictlyOrdered;
    private boolean borrowed;
    public boolean borrowed() { return borrowed; }
    private Batch last;
    private NativeSceneFrame frame;
    private SceneUniforms uniforms;
    private FeatureFrameContext context;
    private FeatureRendererMap renderers;
    private VertexSorting quadSorting;
    private final FeatureRenderPhase.Output phaseOutput=new FeatureRenderPhase.Output() {
        @Override public void accept(SubmitNode node,boolean ordered) {
            if(!group.isEmpty() && (group.getFirst().featureType()!=node.featureType() || strictlyOrdered!=ordered)) buildGroup();
            strictlyOrdered=ordered; group.add(node);
        }
    };

    public void capture(SubmitNodeStorage storage,FeatureRendererMap renderers,FeatureFrameContext context,
            NativeSceneFrame frame,SceneUniforms uniforms,int width,int height,boolean oit,int phaseOverride) {
        if(borrowed || active!=null) throw new IllegalStateException("Feature geometry is still borrowed by a frame");
        borrowed=true;
        this.phaseOverride=phaseOverride;
        quadSorting=quadSorting(phaseOverride,Minecraft.getInstance().gameRenderer.useImprovedTransparency());
        this.frame=frame; this.uniforms=uniforms; this.context=context; this.renderers=renderers; this.width=width; this.height=height;
        try {
            for(var collection:storage.getSubmitsPerOrder().values()) phase(collection.solid,NativeSceneFrame.SOLID);
            if(oit) {
                for(var collection:storage.getSubmitsPerOrder().values()) {
                    phase(collection.waterMask,NativeSceneFrame.WATER_MASK);
                    phase(collection.oitTranslucent,NativeSceneFrame.OIT_BOUNDS);
                }
            } else {
            for(var collection:storage.getSubmitsPerOrder().values()) {
                phase(collection.shadows,NativeSceneFrame.TRANSLUCENT); phase(collection.translucentModels,NativeSceneFrame.TRANSLUCENT);
                phase(collection.nameTags,NativeSceneFrame.TRANSLUCENT); phase(collection.texts,NativeSceneFrame.TRANSLUCENT);
                phase(collection.translucentCustomGeometry,NativeSceneFrame.TRANSLUCENT);
            }
            for(var collection:storage.getSubmitsPerOrder().values()) {
                phase(collection.shapeOutlines,NativeSceneFrame.TRANSLUCENT); phase(collection.translucentGizmos,NativeSceneFrame.TRANSLUCENT);
            }
            for(var collection:storage.getSubmitsPerOrder().values()) {
                phase(collection.translucentBlocksAndItems,NativeSceneFrame.TRANSLUCENT); phase(collection.breakingOverlay,NativeSceneFrame.TRANSLUCENT);
                phase(collection.waterMask,NativeSceneFrame.WATER_MASK);
            }
            }
            for(var collection:storage.getSubmitsPerOrder().values()) phase(collection.afterTerrain,NativeSceneFrame.AFTER_TERRAIN);
            phase(storage.seeThrough(),NativeSceneFrame.SEE_THROUGH);
            for(var collection:storage.getSubmitsPerOrder().values()) {
                phase(collection.alwaysOnTopGizmos,NativeSceneFrame.ALWAYS_ON_TOP);
                phase(collection.outline,NativeSceneFrame.OUTLINE);
            }
        } catch(RuntimeException | Error failure) { release(); throw failure; }
        finally { this.context=null; this.renderers=null; this.uniforms=null; this.frame=null; quadSorting=null; }
    }
    static VertexSorting quadSorting(int viewPhase,boolean improvedTransparency) {
        // Vanilla's global OIT setting also controls index sorting in hand and GUI views.
        return improvedTransparency?null:viewPhase==NativeSceneFrame.OFFSCREEN?VertexSorting.ORTHOGRAPHIC_Z:VertexSorting.DISTANCE_TO_ORIGIN;
    }
    private void phase(FeatureRenderPhase<?> source,int phase) {
        this.phase=phaseOverride<0?phase:phaseOverride;
        source.sortInto(phaseOutput);
        buildGroup();
    }
    private void buildGroup() {
        if(group.isEmpty()) return;
        groupStart=used; last=null; reorderable.clear();
        var renderer=renderers.getOrThrow(group.getFirst().featureType());
        if(renderer instanceof RenderTypeFeatureRenderer<?>) {
            active=this;
            try { ((FeatureBuildAccess)renderer).raynium$build(context,group); }
            finally { active=null; }
            for(int i=groupStart;i<used;i++) finish(batches.get(i));
        } else if(renderer instanceof QuadParticleFeatureRenderer) particles();
        else throw new IllegalArgumentException("Unmapped vanilla feature renderer: "+renderer.getClass().getName());
        group.clear(); reorderable.clear(); last=null;
    }
    private Batch batch(PrimitiveTopology topology,com.mojang.renderpearl.api.vertex.VertexFormat format) {
        if(used==batches.size()) batches.add(new Batch());
        var batch=batches.get(used++); batch.builder=new BufferBuilder(batch.vertices,topology,format); return batch;
    }
    public VertexConsumer vertices(RenderType type) {
        if(active!=this) throw new IllegalStateException("Feature vertex builder outside CPU preparation");
        if(last!=null && last.type==type && type.canConsolidateConsecutiveGeometry()) return last.builder;
        var existing=!strictlyOrdered && type.canConsolidateConsecutiveGeometry()?reorderable.get(type):null;
        if(existing!=null) { last=existing; return existing.builder; }
        last=batch(type.primitiveTopology(),type.format()); last.type=type;
        if(!strictlyOrdered && type.canConsolidateConsecutiveGeometry()) reorderable.put(type,last);
        return last.builder;
    }
    private void finish(Batch batch) {
        batch.mesh=batch.builder.build(); batch.builder=null;
        if(batch.mesh==null) return;
        if(quadSorting!=null && batch.type.sortOnUpload()) batch.mesh.sortQuads(batch.indices,quadSorting);
        var setup=((RenderTypeSceneAccess)(Object)batch.type).raynium$setup(); var access=(RenderSetupSceneAccess)(Object)setup;
        var view=uniforms.view(); var modifier=access.raynium$layering().getModifier(); if(modifier!=null) modifier.accept(view);
        int transforms=uniforms.transforms(view,access.raynium$texturing().createMatrix());
        var client=Minecraft.getInstance();
        var textures=setup.prepareTextures(context.textureManager(),RenderSystem.getSamplerCache(),client.gameRenderer.overlayTexture().getTextureView(),context.lightmap());
        int stages=phase==NativeSceneFrame.OIT_BOUNDS?3:1;
        for(int stage=0;stage<stages;stage++) {
            if(stages==3 && access.raynium$oit()==null) throw new IllegalArgumentException("Missing OIT pipelines: "+batch.type);
            var program=ScenePrograms.require(stages==3?access.raynium$oit().getPipeline(OIT_STAGES[stage]):batch.type.pipeline());
            int first=frame.bindingCount(); uniforms.bind(program,transforms);
            for(var texture:textures) program.texture(frame,texture.name(),texture.textureView(),texture.sampler());
            if(stages==3) program.oitTextures(frame);
            emit(batch,program,first,phase+stage);
        }
    }
    private void emit(Batch batch,ScenePrograms.Program program,int first,int outputPhase) {
        var mesh=batch.mesh; var state=mesh.drawState();
        var index=mesh.indexBuffer();
        if(batch.vertexBlob==-1) {
            batch.vertexBlob=frame.borrow(MemorySegment.ofBuffer(mesh.vertexBuffer()));
            if(index!=null) batch.indexBlob=frame.borrow(MemorySegment.ofBuffer(index));
        }
        boolean points=state.primitiveTopology()==PrimitiveTopology.POINTS;
        frame.draw(program.id(),outputPhase,batch.vertexBlob,batch.indexBlob,state.vertexCount(),index==null?0:state.indexCount(),index==null?0:state.indexType().bytes,
            first,points?state.vertexCount():1,-1,0,0,0,0,0,width,height);
    }
    private void particles() {
        particleBatches.clear();
        for(var value:group) {
            var submit=(QuadParticleFeatureRenderer.Submit)value;
            for(var layer:submit.particles().layers()) if(layer.translucent()==submit.translucent()) {
                var batch=particleBatches.computeIfAbsent(layer,ignored->batch(PrimitiveTopology.QUADS,DefaultVertexFormat.PARTICLE));
                submit.particles().buildLayer(layer,batch.builder);
            }
        }
        int transforms=uniforms.transforms(uniforms.view(),new Matrix4f());
        for(var entry:particleBatches.entrySet()) {
            var batch=entry.getValue(); batch.mesh=batch.builder.build(); batch.builder=null; if(batch.mesh==null) continue;
            var texture=context.textureManager().getTexture(entry.getKey().textureAtlasLocation());
            int stages=phase==NativeSceneFrame.OIT_BOUNDS?3:1;
            for(int stage=0;stage<stages;stage++) {
                var layer=entry.getKey();
                if(stages==3 && layer.oitPipelineSet()==null) throw new IllegalArgumentException("Missing particle OIT pipelines: "+layer);
                var program=ScenePrograms.require(stages==3?layer.oitPipelineSet().getPipeline(OIT_STAGES[stage]):layer.pipeline());
                int first=frame.bindingCount(); uniforms.bind(program,transforms);
                program.texture(frame,"Sampler0",texture.getTextureView(),texture.getSampler());
                program.texture(frame,"Sampler2",context.lightmap(),RenderSystem.getSamplerCache().getClampToEdge(FilterMode.LINEAR));
                if(stages==3) program.oitTextures(frame);
                emit(batch,program,first,phase+stage);
            }
        }
        particleBatches.clear();
    }
    public void release() {
        for(int i=0;i<used;i++) batches.get(i).release();
        used=0; borrowed=false; group.clear(); reorderable.clear(); particleBatches.clear(); last=null;
    }
    @Override public void close() { release(); for(var batch:batches) batch.close(); batches.clear(); }
}
