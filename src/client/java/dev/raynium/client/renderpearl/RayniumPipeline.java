package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.backend.api.BackendRenderPipeline;
import com.mojang.renderpearl.frontend.FrontendRenderPipeline;
import dev.raynium.bridge.NativeRenderer;
import it.unimi.dsi.fastutil.objects.Object2IntOpenHashMap;
import java.util.List;

final class RayniumPipeline implements BackendRenderPipeline {
    final NativeRenderer owner;
    final int id;
    final int pushBytes;
    final List<RayniumPrograms.Route> routes;
    final PrimitiveTopology topology;
    private int recording;
    private boolean closed;

    private RayniumPipeline(NativeRenderer owner,RayniumPrograms.Preparation prepared,RenderPipeline pipeline) {
        this.owner=owner;
        this.topology=pipeline.getPrimitiveTopology();
        pushBytes=pipeline.pushConstantSize();
        routes=prepared.routes();
        id=owner.registerPipeline(prepared.name(),prepared.words(),prepared.pack());
    }

    static FrontendRenderPipeline register(NativeRenderer owner,RenderPipeline pipeline,RayniumPrograms.Preparation prepared) {
        var indices=new Object2IntOpenHashMap<String>();
        indices.defaultReturnValue(-1);
        for(var name:prepared.uniforms()) indices.put(name,indices.size());
        return new FrontendRenderPipeline(pipeline.getLocation().toString(),new RayniumPipeline(owner,prepared,pipeline),
            pipeline.getVertexFormatBindings(),indices,BindGroupLayout.flattenUniforms(pipeline.getBindGroupLayouts()),
            pipeline.getColorTargetStates(),pipeline.getDepthStencilState()!=null,pipeline.pushConstantSize());
    }

    void retain() {
        if(closed) throw new IllegalStateException("Pipeline is closed");
        ++recording;
    }

    void release() {
        if(--recording==0 && closed) owner.destroyPipeline(id);
    }

    @Override public boolean isClosed() { return closed; }
    @Override public void close() {
        if(closed) return;
        closed=true;
        if(recording==0) owner.destroyPipeline(id);
    }
}
