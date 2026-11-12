package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Arrays;
import static java.lang.foreign.ValueLayout.*;

/** Render-thread scratch storage. Reserve all capacities before writing; pointers expire on growth or close. */
public final class NativeFrameBuffer implements AutoCloseable {
    private static final class Storage implements AutoCloseable {
        private Arena arena;
        MemorySegment segment=MemorySegment.NULL;
        void reserve(long bytes) {
            reserve(bytes,false);
        }
        void reserve(long bytes,boolean preserve) {
            if(bytes<=segment.byteSize()) return;
            long capacity=8;
            while(capacity<bytes) capacity=Math.multiplyExact(capacity,2);
            var next=Arena.ofConfined();
            MemorySegment replacement;
            try { replacement=next.allocate(capacity,8); }
            catch(RuntimeException | Error failure) { next.close(); throw failure; }
            if(preserve && segment.byteSize()!=0) MemorySegment.copy(segment,0,replacement,0,segment.byteSize());
            close(); arena=next; segment=replacement;
        }
        @Override public void close() { if(arena!=null) { arena.close(); arena=null; segment=MemorySegment.NULL; } }
    }
    private final Arena fixed=Arena.ofConfined();
    private final MemorySegment descriptor=fixed.allocate(328,8);
    private final Storage[] arrays=new Storage[5];
    private final Storage upload=new Storage();
    private final Storage guiVertices=new Storage(),guiDraws=new Storage();
    private int guiVertexCount,guiDrawCount;
    private final int[] counts=new int[5];
    private long uploadCursor;
    private boolean closed;

    public NativeFrameBuffer() { for(int i=0;i<arrays.length;i++) arrays[i]=new Storage(); }

    public void begin(int visibleCapacity,int removedCapacity,int meshCapacity,int indexCapacity,long uploadBytes) {
        if(closed) throw new IllegalStateException("Frame buffer is closed");
        arrays[0].reserve(24L*indexCapacity); arrays[1].reserve(24L*meshCapacity);
        arrays[2].reserve(8L*Math.max(256,removedCapacity));
        arrays[3].reserve(24L*Math.max(1024,visibleCapacity)); arrays[4].reserve(16L*Math.max(1024,visibleCapacity));
        upload.reserve(uploadBytes); Arrays.fill(counts,0); uploadCursor=0; descriptor.fill((byte)0);
        guiVertexCount=guiDrawCount=0;
    }
    public void terrain(int albedo,int light,int anisotropy) {
        descriptor.set(JAVA_INT,84,albedo); descriptor.set(JAVA_INT,88,light); descriptor.set(JAVA_INT,92,anisotropy);
    }
    private long dataOffset(long bytes) {
        long offset=(uploadCursor+7)&~7L;
        uploadCursor=Math.addExact(offset,bytes);
        if(uploadCursor>upload.segment.byteSize()) throw new IllegalStateException("Upload capacity was not reserved");
        return offset;
    }
    public static long alignedBytes(long bytes) { return Math.addExact(bytes,7)&~7L; }
    private void indexHeader(long id,long offset,int count) {
        var s=arrays[0].segment; long o=counts[0]++*24L;
        s.set(JAVA_LONG,o,id); s.set(ADDRESS,o+8,upload.segment.asSlice(offset));
        s.set(JAVA_INT,o+16,count); s.set(JAVA_INT,o+20,0);
    }
    public void index(long id,int[] values) {
        long offset=dataOffset(values.length*4L);
        MemorySegment.copy(MemorySegment.ofArray(values),0,upload.segment,offset,values.length*4L);
        indexHeader(id,offset,values.length);
    }
    public void mesh(long id,byte[] vertices,int vertexCount,int layer) {
        if(vertexCount*28L!=vertices.length) throw new IllegalArgumentException("Vertex byte count mismatch");
        long offset=dataOffset(vertices.length);
        MemorySegment.copy(MemorySegment.ofArray(vertices),0,upload.segment,offset,vertices.length);
        var s=arrays[1].segment; long o=counts[1]++*24L;
        s.set(JAVA_LONG,o,id); s.set(ADDRESS,o+8,upload.segment.asSlice(offset));
        s.set(JAVA_INT,o+16,vertexCount); s.set(JAVA_INT,o+20,layer);
    }
    public void visible(long mesh,int elementMask,int sortKey,int firstInstance,int instanceCount) {
        var s=arrays[3].segment; long o=counts[3]++*24L;
        s.set(JAVA_LONG,o,mesh); s.set(JAVA_INT,o+8,elementMask); s.set(JAVA_INT,o+12,sortKey);
        s.set(JAVA_INT,o+16,firstInstance); s.set(JAVA_INT,o+20,instanceCount);
    }
    public int instance(int x,int y,int z,float visibility) {
        int index=counts[4]++; var s=arrays[4].segment; long o=index*16L;
        s.set(JAVA_INT,o,x); s.set(JAVA_INT,o+4,y); s.set(JAVA_INT,o+8,z); s.set(JAVA_FLOAT,o+12,visibility);
        return index;
    }
    private void removed(int array,long id) { arrays[array].segment.set(JAVA_LONG,counts[array]++*8L,id); }
    public void removeMesh(long id) { removed(2,id); }
    public int visibleCount() { return counts[3]; }
    public void beginGui(int vertices,int draws) {
        if(closed) throw new IllegalStateException("Frame buffer is closed");
        guiVertices.reserve(Math.multiplyExact(24L,vertices)); guiDraws.reserve(Math.multiplyExact(40L,draws));
        guiVertexCount=guiDrawCount=0;
    }
    public void guiVertex(float x,float y,float z,float u,float v,int color) {
        guiVertices.reserve(Math.multiplyExact(24L,guiVertexCount+1),true);
        long offset=guiVertexCount++*24L; var s=guiVertices.segment;
        s.set(JAVA_FLOAT,offset,x); s.set(JAVA_FLOAT,offset+4,y); s.set(JAVA_FLOAT,offset+8,z);
        s.set(JAVA_FLOAT,offset+12,u); s.set(JAVA_FLOAT,offset+16,v); s.set(JAVA_INT,offset+20,color);
    }
    public int guiVertexCount() { return guiVertexCount; }
    public void guiDraw(int pipeline,int texture,int samplerBits,int first,int count,int x,int y,int width,int height) {
        guiDraws.reserve(Math.multiplyExact(40L,guiDrawCount+1),true);
        long offset=guiDrawCount++*40L; var s=guiDraws.segment;
        s.set(JAVA_INT,offset,pipeline); s.set(JAVA_INT,offset+4,texture); s.set(JAVA_INT,offset+8,samplerBits);
        s.set(JAVA_INT,offset+12,first); s.set(JAVA_INT,offset+16,count); s.set(JAVA_INT,offset+20,x);
        s.set(JAVA_INT,offset+24,y); s.set(JAVA_INT,offset+28,width); s.set(JAVA_INT,offset+32,height); s.set(JAVA_INT,offset+36,0);
    }
    private static void floats(MemorySegment target,long offset,float[] values,int count) {
        if(values.length!=count) throw new IllegalArgumentException("Expected "+count+" floats");
        for(int i=0;i<count;i++) target.set(JAVA_FLOAT,offset+i*4L,values[i]);
    }
    public MemorySegment finish(NativeRenderer.Frame frame) {
        var s=descriptor; s.set(JAVA_INT,0,328); s.set(JAVA_INT,4,frame.flags);
        s.set(JAVA_LONG,8,frame.epoch); s.set(JAVA_LONG,16,frame.outputSequence);
        for(int i=0;i<5;i++) {
            s.set(ADDRESS,24+i*8L,counts[i]==0?MemorySegment.NULL:arrays[i].segment);
            s.set(JAVA_INT,64+i*4L,counts[i]);
        }
        floats(s,96,frame.projection,16); floats(s,160,frame.view,16);
        for(int i=0;i<3;i++) s.set(JAVA_INT,224+i*4L,frame.cameraBlock[i]);
        floats(s,236,frame.cameraFraction,3); floats(s,248,frame.fogColor,4); floats(s,264,frame.fogDistances,4);
        s.set(ADDRESS,280,guiVertexCount==0?MemorySegment.NULL:guiVertices.segment);
        s.set(ADDRESS,288,guiDrawCount==0?MemorySegment.NULL:guiDraws.segment);
        s.set(JAVA_INT,296,guiVertexCount); s.set(JAVA_INT,300,guiDrawCount);
        s.set(JAVA_FLOAT,304,frame.guiWidth); s.set(JAVA_FLOAT,308,frame.guiHeight);
        if(frame.clearColor==null) {
            for(int i=0;i<3;i++) s.set(JAVA_FLOAT,312+i*4L,frame.fogColor[i]); s.set(JAVA_FLOAT,324,0);
        } else floats(s,312,frame.clearColor,4);
        return s;
    }
    MemorySegment marshal(NativeRenderer.Frame frame) {
        long bytes=0;
        for(var x:frame.indices) bytes=Math.addExact(bytes,alignedBytes(x.values().length*4L));
        for(var x:frame.meshes) bytes=Math.addExact(bytes,alignedBytes(x.vertices().length));
        begin(Math.max(frame.visible.size(),frame.instances.size()),frame.removedMeshes.length,frame.meshes.size(),frame.indices.size(),bytes);
        terrain(frame.terrainAlbedoTexture,frame.terrainLightTexture,frame.terrainAnisotropy);
        for(var x:frame.indices) index(x.meshId(),x.values());
        for(var x:frame.meshes) mesh(x.id(),x.vertices(),x.vertexCount(),x.layer());
        for(var x:frame.visible) visible(x.mesh(),x.elementMask(),x.sortKey(),x.firstInstance(),x.instanceCount());
        for(var x:frame.instances) instance(x.x(),x.y(),x.z(),x.visibility());
        for(long id:frame.removedMeshes) removeMesh(id);
        if(frame.guiVertices.length%24!=0) throw new IllegalArgumentException("GUI vertex byte count mismatch");
        beginGui(frame.guiVertices.length/24,frame.guiDraws.size());
        MemorySegment.copy(MemorySegment.ofArray(frame.guiVertices),0,guiVertices.segment,0,frame.guiVertices.length);
        guiVertexCount=frame.guiVertices.length/24;
        for(var draw:frame.guiDraws) guiDraw(draw.pipeline(),draw.texture(),draw.samplerBits(),draw.firstVertex(),draw.vertexCount(),draw.x(),draw.y(),draw.width(),draw.height());
        return finish(frame);
    }
    @Override public void close() {
        if(closed) return;
        for(var storage:arrays) storage.close(); upload.close(); guiVertices.close(); guiDraws.close(); fixed.close(); closed=true;
    }
}
