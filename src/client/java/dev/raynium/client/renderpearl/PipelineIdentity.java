package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.api.vertex.VertexFormatElement;
import java.util.*;
import net.minecraft.resources.Identifier;

/** Immutable program and raster state; resource bindings and attachment formats are separate cache keys. */
public record PipelineIdentity(Map<ShaderType,Identifier> shaders,Map<String,String> defineValues,Set<String> defineFlags,
        List<BindGroupLayout.UniformDescription> uniforms,List<VertexBinding> vertices,
        PrimitiveTopology topology,PolygonMode polygonMode,boolean cull,DepthStencilState depth,
        List<ColorTargetState> colors,int pushConstantSize) {
    public record VertexBinding(int stride,int stepRate,List<VertexFormatElement> elements) {
        public VertexBinding { elements=List.copyOf(elements); }
    }
    public PipelineIdentity {
        shaders=Map.copyOf(shaders); defineValues=Map.copyOf(defineValues); defineFlags=Set.copyOf(defineFlags);
        uniforms=List.copyOf(uniforms);
        vertices=Collections.unmodifiableList(new ArrayList<>(vertices));
        colors=Collections.unmodifiableList(new ArrayList<>(colors));
    }
    public static PipelineIdentity of(RenderPipeline pipeline) {
        var formats=pipeline.getVertexFormatBindings();
        var vertices=new ArrayList<VertexBinding>(formats.size());
        for(var format:formats)
            vertices.add(format==null?null:new VertexBinding(format.getVertexSize(),format.getStepRate(),format.getElements()));
        var uniforms=new ArrayList<>(BindGroupLayout.flattenUniforms(pipeline.getBindGroupLayouts()));
        uniforms.sort(Comparator.comparing(BindGroupLayout.UniformDescription::name));
        return new PipelineIdentity(pipeline.getShaders(),pipeline.getShaderDefines().values(),pipeline.getShaderDefines().flags(),
            uniforms,vertices,pipeline.getPrimitiveTopology(),pipeline.getPolygonMode(),pipeline.isCull(),
            pipeline.getDepthStencilState(),pipeline.getColorTargetStates(),pipeline.pushConstantSize());
    }
}
