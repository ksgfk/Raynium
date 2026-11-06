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
            if(bytes<=segment.byteSize()) return;
            long capacity=8;
            while(capacity<bytes) capacity=Math.multiplyExact(capacity,2);
            var next=Arena.ofConfined();
            MemorySegment replacement;
            try { replacement=next.allocate(capacity,8); }
            catch(RuntimeException | Error failure) { next.close(); throw failure; }
            close(); arena=next; segment=replacement;
        }
        @Override public void close() { if(arena!=null) { arena.close(); arena=null; segment=MemorySegment.NULL; } }
    }
    private final Arena fixed=Arena.ofConfined();
    private final MemorySegment descriptor=fixed.allocate(344,8),error=fixed.allocate(1024,8);
    private final Storage[] arrays=new Storage[10];
    private final Storage submeshes=new Storage(),upload=new Storage();
    private final int[] counts=new int[10];
    private long uploadCursor,submeshCursor;
    private boolean closed;

    public NativeFrameBuffer() { for(int i=0;i<arrays.length;i++) arrays[i]=new Storage(); }

    public void begin(int visibleCapacity,int removedCapacity,int meshCapacity,int submeshCapacity,
                      int materialCapacity,int indexCapacity,long uploadBytes) {
        if(closed) throw new IllegalStateException("Frame buffer is closed");
        arrays[0].reserve(24L*indexCapacity); arrays[1].reserve(40L*meshCapacity);
        arrays[2].reserve(24L*materialCapacity); arrays[3].reserve(24L*meshCapacity);
        for(int i=4;i<8;i++) arrays[i].reserve(8L*Math.max(256,removedCapacity));
        arrays[8].reserve(24L*Math.max(1024,visibleCapacity)); arrays[9].reserve(16L*Math.max(1024,visibleCapacity));
        submeshes.reserve(16L*submeshCapacity); upload.reserve(uploadBytes);
        Arrays.fill(counts,0); uploadCursor=submeshCursor=0; descriptor.fill((byte)0);
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
    public void quadIndices(long id,int vertices) {
        if(vertices<4 || vertices%4!=0) throw new IllegalArgumentException("Expected complete quads");
        int count=Math.multiplyExact(vertices/4,6); long offset=dataOffset(count*4L),o=offset;
        for(int v=0;v<vertices;v+=4) {
            upload.segment.set(JAVA_INT,o,v); upload.segment.set(JAVA_INT,o+4,v+1); upload.segment.set(JAVA_INT,o+8,v+2);
            upload.segment.set(JAVA_INT,o+12,v+2); upload.segment.set(JAVA_INT,o+16,v+3); upload.segment.set(JAVA_INT,o+20,v); o+=24;
        }
        indexHeader(id,offset,count);
    }
    public void mesh(long id,byte[] vertices,long index,int vertexCount,int submeshCount) {
        if(vertexCount*28L!=vertices.length) throw new IllegalArgumentException("Vertex byte count mismatch");
        long offset=dataOffset(vertices.length);
        MemorySegment.copy(MemorySegment.ofArray(vertices),0,upload.segment,offset,vertices.length);
        var s=arrays[1].segment; long o=counts[1]++*40L;
        s.set(JAVA_LONG,o,id); s.set(ADDRESS,o+8,upload.segment.asSlice(offset)); s.set(JAVA_LONG,o+16,index);
        s.set(ADDRESS,o+24,submeshCount==0?MemorySegment.NULL:submeshes.segment.asSlice(submeshCursor));
        s.set(JAVA_INT,o+32,vertexCount); s.set(JAVA_INT,o+36,submeshCount);
    }
    public void submesh(int first,int count,int baseVertex) {
        var s=submeshes.segment; long o=submeshCursor; submeshCursor+=16;
        s.set(JAVA_INT,o,first); s.set(JAVA_INT,o+4,count); s.set(JAVA_INT,o+8,baseVertex); s.set(JAVA_INT,o+12,0);
    }
    public void material(long id,int albedo,int light,int anisotropy) {
        var s=arrays[2].segment; long o=counts[2]++*24L;
        s.set(JAVA_LONG,o,id); s.set(JAVA_INT,o+8,albedo); s.set(JAVA_INT,o+12,light);
        s.set(JAVA_INT,o+16,anisotropy); s.set(JAVA_INT,o+20,0);
    }
    public void primitive(long id,long mesh,long material) {
        var s=arrays[3].segment; long o=counts[3]++*24L;
        s.set(JAVA_LONG,o,id); s.set(JAVA_LONG,o+8,mesh); s.set(JAVA_LONG,o+16,material);
    }
    public void visible(long primitive,int elementMask,int sortKey,int firstInstance,int instanceCount) {
        var s=arrays[8].segment; long o=counts[8]++*24L;
        s.set(JAVA_LONG,o,primitive); s.set(JAVA_INT,o+8,elementMask); s.set(JAVA_INT,o+12,sortKey);
        s.set(JAVA_INT,o+16,firstInstance); s.set(JAVA_INT,o+20,instanceCount);
    }
    public int instance(int x,int y,int z,float visibility) {
        int index=counts[9]++; var s=arrays[9].segment; long o=index*16L;
        s.set(JAVA_INT,o,x); s.set(JAVA_INT,o+4,y); s.set(JAVA_INT,o+8,z); s.set(JAVA_FLOAT,o+12,visibility);
        return index;
    }
    private void removed(int array,long id) { arrays[array].segment.set(JAVA_LONG,counts[array]++*8L,id); }
    public void removeMesh(long id) { removed(4,id); }
    public void removePrimitive(long id) { removed(5,id); }
    public int visibleCount() { return counts[8]; }
    MemorySegment error() { return error; }
    private static void floats(MemorySegment target,long offset,float[] values,int count) {
        if(values.length!=count) throw new IllegalArgumentException("Expected "+count+" floats");
        for(int i=0;i<count;i++) target.set(JAVA_FLOAT,offset+i*4L,values[i]);
    }
    public MemorySegment finish(NativeRenderer.Frame frame) {
        var s=descriptor; s.set(JAVA_INT,0,344); s.set(JAVA_INT,4,frame.flags);
        s.set(JAVA_LONG,8,frame.epoch); s.set(JAVA_LONG,16,frame.outputSequence); s.set(JAVA_LONG,24,frame.inputSequence);
        for(int i=0;i<10;i++) {
            s.set(ADDRESS,32+i*8L,counts[i]==0?MemorySegment.NULL:arrays[i].segment);
            s.set(JAVA_INT,112+i*4L,counts[i]);
        }
        s.set(JAVA_INT,152,frame.changedTextures); floats(s,160,frame.projection,16); floats(s,224,frame.view,16);
        if(frame.cameraBlock.length!=3) throw new IllegalArgumentException("Expected three camera coordinates");
        for(int i=0;i<3;i++) s.set(JAVA_INT,288+i*4L,frame.cameraBlock[i]);
        floats(s,300,frame.cameraFraction,3); floats(s,312,frame.fogColor,4); floats(s,328,frame.fogDistances,4);
        return s;
    }
    MemorySegment marshal(NativeRenderer.Frame frame) {
        long bytes=0; int ranges=0;
        for(int i=0;i<frame.indices.size();i++) bytes=Math.addExact(bytes,alignedBytes(frame.indices.get(i).values().length*4L));
        for(int i=0;i<frame.meshes.size();i++) {
            var mesh=frame.meshes.get(i); bytes=Math.addExact(bytes,alignedBytes(mesh.vertices().length));
            ranges=Math.addExact(ranges,mesh.submeshes().size());
        }
        int removed=Math.max(Math.max(frame.removedMeshes.length,frame.removedPrimitives.length),Math.max(frame.removedMaterials.length,frame.removedIndices.length));
        begin(Math.max(frame.visible.size(),frame.instances.size()),removed,Math.max(frame.meshes.size(),frame.primitives.size()),ranges,
            frame.materials.size(),frame.indices.size(),bytes);
        for(int i=0;i<frame.indices.size();i++) { var x=frame.indices.get(i); index(x.id(),x.values()); }
        for(int i=0;i<frame.meshes.size();i++) {
            var x=frame.meshes.get(i); mesh(x.id(),x.vertices(),x.index(),x.vertexCount(),x.submeshes().size());
            for(int j=0;j<x.submeshes().size();j++) { var r=x.submeshes().get(j); submesh(r.first(),r.count(),r.baseVertex()); }
        }
        for(int i=0;i<frame.materials.size();i++) { var x=frame.materials.get(i); material(x.id(),x.albedo(),x.light(),x.anisotropy()); }
        for(int i=0;i<frame.primitives.size();i++) { var x=frame.primitives.get(i); primitive(x.id(),x.mesh(),x.material()); }
        for(int i=0;i<frame.visible.size();i++) { var x=frame.visible.get(i); visible(x.primitive(),x.elementMask(),x.sortKey(),x.firstInstance(),x.instanceCount()); }
        for(int i=0;i<frame.instances.size();i++) { var x=frame.instances.get(i); instance(x.x(),x.y(),x.z(),x.visibility()); }
        for(long id:frame.removedMeshes) removed(4,id);
        for(long id:frame.removedPrimitives) removed(5,id);
        for(long id:frame.removedMaterials) removed(6,id);
        for(long id:frame.removedIndices) removed(7,id);
        return finish(frame);
    }
    @Override public void close() {
        if(closed) return;
        for(var storage:arrays) storage.close(); submeshes.close(); upload.close(); fixed.close(); closed=true;
    }
}
