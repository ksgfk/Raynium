package dev.raynium.client.takeover.scene;

import com.mojang.blaze3d.vertex.*;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import com.mojang.renderpearl.api.vertex.VertexFormat;
import dev.raynium.bridge.NativeSceneFrame;
import java.lang.foreign.MemorySegment;

public final class SceneMesh implements SceneInput,AutoCloseable {
    private final ByteBufferBuilder bytes=new ByteBufferBuilder(32768);
    private MeshData mesh;
    private boolean borrowed;
    public boolean borrowed() { return borrowed; }
    public BufferBuilder begin(PrimitiveTopology topology,VertexFormat format) {
        if(borrowed) throw new IllegalStateException("Scene mesh still borrowed");
        borrowed=true; return new BufferBuilder(bytes,topology,format);
    }
    public int finish(BufferBuilder builder,NativeSceneFrame frame) {
        mesh=builder.buildOrThrow(); return frame.borrow(MemorySegment.ofBuffer(mesh.vertexBuffer()));
    }
    public int vertexCount() { return mesh.drawState().vertexCount(); }
    @Override public void release() { if(mesh!=null) { mesh.close(); mesh=null; } bytes.clear(); borrowed=false; }
    @Override public void close() { release(); bytes.close(); }
}
