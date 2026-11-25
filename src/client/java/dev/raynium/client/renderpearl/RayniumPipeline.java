package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.backend.api.BackendRenderPipeline;
import com.mojang.renderpearl.frontend.FrontendRenderPipeline;
import dev.raynium.bridge.NativeRenderer;
import it.unimi.dsi.fastutil.objects.Object2IntOpenHashMap;
import java.util.List;

final class RayniumPipeline implements BackendRenderPipeline {
    final NativeRenderer owner;
    final long handle;
    final long instancedHandle;
    final String instancedMode;
    final List<RayniumPrograms.Route> instancedRoutes;
    final int[] instancedUniformMap;
    final int pushBytes;
    final List<RayniumPrograms.Route> routes;
    final PrimitiveTopology topology;
    private final Runnable onClose;
    private int recording;
    long lastRetainedTapeId;
    private boolean closed;

    private RayniumPipeline(NativeRenderer owner,RayniumPrograms.Preparation prepared,RenderPipeline pipeline,
            Object2IntOpenHashMap<String> baseIndices,Runnable onClose) {
        this.owner=owner;
        this.topology=pipeline.getPrimitiveTopology();
        this.pushBytes=pipeline.pushConstantSize();
        this.routes=prepared.routes();
        this.onClose=onClose;
        this.handle=owner.registerPipeline(prepared.name(),prepared.words(),prepared.pack());
        var inst=prepared.instanced();
        if(inst!=null) {
            int[] map=new int[inst.uniforms().size()];
            boolean allMapped=true;
            for(int i=0;i<map.length;++i) {
                int baseIndex=baseIndices.getInt(inst.uniforms().get(i));
                if(baseIndex<0) { allMapped=false; break; }
                map[i]=baseIndex;
            }
            if(allMapped) {
                this.instancedHandle=owner.registerPipeline(inst.name(),inst.words(),null);
                this.instancedMode=inst.mode();
                this.instancedRoutes=inst.routes();
                this.instancedUniformMap=map;
            } else {
                this.instancedHandle=0;
                this.instancedMode=null;
                this.instancedRoutes=List.of();
                this.instancedUniformMap=new int[0];
            }
        } else {
            this.instancedHandle=0;
            this.instancedMode=null;
            this.instancedRoutes=List.of();
            this.instancedUniformMap=new int[0];
        }
    }

    static FrontendRenderPipeline register(NativeRenderer owner,RenderPipeline pipeline,RayniumPrograms.Preparation prepared,
            java.util.function.BiConsumer<RenderPipeline,RayniumPipeline> onRegister,
            java.util.function.BiConsumer<RenderPipeline,RayniumPipeline> onRelease) {
        var indices=new Object2IntOpenHashMap<String>();
        indices.defaultReturnValue(-1);
        for(var name:prepared.uniforms()) indices.put(name,indices.size());
        var ref=new RayniumPipeline[1];
        var backend=new RayniumPipeline(owner,prepared,pipeline,indices,()->{ if(onRelease!=null) onRelease.accept(pipeline,ref[0]); });
        ref[0]=backend;
        if(onRegister!=null) onRegister.accept(pipeline,backend);
        return new FrontendRenderPipeline(pipeline.getLocation().toString(),backend,
            pipeline.getVertexFormatBindings(),indices,BindGroupLayout.flattenUniforms(pipeline.getBindGroupLayouts()),
            pipeline.getColorTargetStates(),pipeline.getDepthStencilState()!=null,pipeline.pushConstantSize());
    }

    boolean supportsInstanced(String mode) {
        return !closed && instancedHandle!=0 && mode.equals(instancedMode);
    }

    void retain() {
        if(closed) throw new IllegalStateException("Pipeline is closed");
        ++recording;
    }

    void release() {
        if(--recording==0 && closed) destroyNative();
    }

    private void destroyNative() {
        owner.destroyPipeline(handle);
        if(instancedHandle!=0) owner.destroyPipeline(instancedHandle);
    }

    @Override public boolean isClosed() { return closed; }
    @Override public void close() {
        if(closed) return;
        closed=true;
        if(onClose!=null) onClose.run();
        if(recording==0) destroyNative();
    }
}
