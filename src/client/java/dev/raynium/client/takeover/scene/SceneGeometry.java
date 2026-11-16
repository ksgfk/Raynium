package dev.raynium.client.takeover.scene;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import dev.raynium.bridge.NativeSceneBuffer;
import dev.raynium.client.takeover.TakeoverRuntime;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;

/** Original CPU geometry uploaded once; the owning renderer closes it on rebuild or reload. */
public final class SceneGeometry implements GpuBuffer {
    private final NativeSceneBuffer vertices;
    private NativeSceneBuffer indices;
    private final int usage;
    private int indexCount,vertexCount;
    private PrimitiveTopology topology;
    private boolean closed;
    public SceneGeometry(int usage,long size) { this.usage=usage; vertices=TakeoverRuntime.renderer().createSceneBuffer(size); }
    public SceneGeometry(int usage,ByteBuffer data) {
        this.usage=usage; vertices=TakeoverRuntime.renderer().createSceneBuffer(data.remaining());
        try { vertices.upload(0,MemorySegment.ofBuffer(data)); }
        catch(RuntimeException | Error failure) { vertices.close(); throw failure; }
    }
    public int reference() { return vertices.reference(); }
    public void upload(long offset,ByteBuffer bytes) { vertices.upload(offset,MemorySegment.ofBuffer(bytes)); }
    public int indices(PrimitiveTopology type,int count) {
        if(indices!=null) {
            if(type!=topology || count!=vertexCount) throw new IllegalArgumentException("Static geometry topology changed");
            return indices.reference();
        }
        int[] values=sequentialIndices(type,count);
        var buffer=TakeoverRuntime.renderer().createSceneBuffer(values.length*4L);
        try { buffer.upload(0,MemorySegment.ofArray(values)); }
        catch(RuntimeException | Error failure) { buffer.close(); throw failure; }
        indices=buffer; indexCount=values.length; vertexCount=count; topology=type; return buffer.reference();
    }
    static int[] sequentialIndices(PrimitiveTopology type,int count) {
        if(type==PrimitiveTopology.LINES && count>=4 && count%4==0) {
            int[] result=new int[Math.multiplyExact(count/4,6)];
            for(int i=0;i<count/4;i++) { int at=i*6,v=i*4; result[at]=v; result[at+1]=v+1; result[at+2]=v+2;
                result[at+3]=v+3; result[at+4]=v+2; result[at+5]=v+1; }
            return result;
        }
        if(type==PrimitiveTopology.QUADS && count>=4 && count%4==0) {
            int[] result=new int[Math.multiplyExact(count/4,6)];
            for(int i=0;i<count/4;i++) { int at=i*6,v=i*4; result[at]=v; result[at+1]=v+1; result[at+2]=v+2;
                result[at+3]=v+2; result[at+4]=v+3; result[at+5]=v; }
            return result;
        }
        if(type==PrimitiveTopology.TRIANGLE_FAN && count>=3) {
            int[] result=new int[Math.multiplyExact(count-2,3)];
            for(int i=0;i<count-2;i++) { result[i*3+1]=i+1; result[i*3+2]=i+2; } return result;
        }
        throw new IllegalArgumentException("Unsupported static geometry topology/count: "+type+"/"+count);
    }
    public int indexCount() { return indexCount; }
    @Override public long size() { return vertices.size(); }
    @Override public int usage() { return usage; }
    @Override public boolean isClosed() { return closed; }
    @Override public GpuBufferSlice.MappedView map(long offset,long length,boolean read,boolean write) { throw new IllegalStateException("Device-local scene geometry cannot be mapped"); }
    @Override public void close() { if(!closed) { if(indices!=null) indices.close(); vertices.close(); closed=true; } }
}
