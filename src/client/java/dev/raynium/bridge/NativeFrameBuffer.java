package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.Arrays;
import static java.lang.foreign.ValueLayout.*;

/** Render-thread scratch storage. Reserve all capacities before writing; pointers expire on growth or close. */
public final class NativeFrameBuffer implements AutoCloseable {
    static final class Storage implements AutoCloseable {
        private Arena arena;
        private int lowUsageFrames;
        MemorySegment segment=MemorySegment.NULL;
        void reserve(long bytes) {
            if(segment.byteSize()>16L*1024*1024 && bytes<segment.byteSize()/4) {
                if(++lowUsageFrames==120) close();
            } else {
                lowUsageFrames=0;
            }
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
        @Override public void close() { lowUsageFrames=0; if(arena!=null) { arena.close(); arena=null; segment=MemorySegment.NULL; } }
    }
    private final Arena fixed=Arena.ofConfined();
    private final MemorySegment descriptor=fixed.allocate(BYTE_SIZE,8);
    private final Storage[] arrays=new Storage[5];
    private final Storage layouts=new Storage();
    private final Storage upload=new Storage();
    private final int[] counts=new int[5];
    private final ArrayList<Object> borrowed=new ArrayList<>();
    private Arena externalCopies;
    private final ArrayList<dev.raynium.client.terrain.MeshPayload.UploadLease> uploads=new ArrayList<>();
    private int layoutCount;
    private long uploadCursor;
    private boolean closed;

    public static final int BYTE_SIZE=280;
    public static final int MESH_UPDATE_STRIDE=32;
    public static final int MESH_LAYOUT_STRIDE=72;
    public static final int LAYOUT_FLAG_FINITE_BOUNDS=1;
    public static final int LAYOUT_FLAG_WIREFRAME_GROUPS=2;

    private final boolean pooledTerrain;
    public NativeFrameBuffer() { this(null); }
    public NativeFrameBuffer(dev.raynium.client.terrain.RetainedTerrain.ViewAccess access) {
        pooledTerrain=access!=null;
        for(int i=0;i<arrays.length;i++) arrays[i]=new Storage();
    }
    public boolean isTerrainStorage() { return pooledTerrain; }
    public boolean isClosed() { return closed; }

    public void begin(int visibleCapacity,int removedCapacity,int meshCapacity,int indexCapacity,long uploadBytes) {
        if(closed) throw new IllegalStateException("Frame buffer is closed");
        clearBorrowed();
        arrays[0].reserve(24L*indexCapacity);
        arrays[1].reserve((long)MESH_UPDATE_STRIDE*meshCapacity);
        arrays[2].reserve(8L*Math.max(256,removedCapacity));
        arrays[3].reserve(24L*Math.max(1024,visibleCapacity));
        arrays[4].reserve(16L*Math.max(1024,visibleCapacity));
        layouts.reserve((long)MESH_LAYOUT_STRIDE*meshCapacity);
        upload.reserve(uploadBytes);
        Arrays.fill(counts,0);
        layoutCount=0;
        uploadCursor=0;
        descriptor.fill((byte)0);
    }
    public void retainBorrowed(Object owner) {
        if(owner!=null) borrowed.add(owner);
    }
    public void clearBorrowed() {
        for(var upload:uploads) upload.close();
        uploads.clear();
        borrowed.clear();
        if(externalCopies!=null) { externalCopies.close(); externalCopies=null; }
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
    private void indexHeader(long id,MemorySegment pointer,int count) {
        var s=arrays[0].segment; long o=counts[0]++*24L;
        s.set(JAVA_LONG,o,id); s.set(ADDRESS,o+8,pointer);
        s.set(JAVA_INT,o+16,count); s.set(JAVA_INT,o+20,0);
    }
    public void index(long id,int[] values) {
        long offset=dataOffset(values.length*4L);
        MemorySegment.copy(MemorySegment.ofArray(values),0,upload.segment,offset,values.length*4L);
        indexHeader(id,upload.segment.asSlice(offset),values.length);
    }
    private MemorySegment own(MemorySegment source,long bytes) {
        if(externalCopies==null) externalCopies=Arena.ofConfined();
        var copy=externalCopies.allocate(bytes,8);
        copy.copyFrom(source.asSlice(0,bytes));
        return copy;
    }
    public void indexPooled(long id,dev.raynium.client.terrain.MeshPayload.IndexSnapshot snapshot) {
        var lease=snapshot.pinIndices();
        uploads.add(lease);
        indexHeader(id,lease.bytes(),snapshot.count);
    }
    public void meshPooled(long id,dev.raynium.client.terrain.MeshPayload mesh) {
        var lease=mesh.pinVertices();
        uploads.add(lease);
        meshRecord(id,lease.bytes(),mesh.vertexCount,mesh.mode,mesh.groupQuadCounts,mesh.layoutFlags,
                mesh.minBounds,mesh.maxBounds,mesh.originalQuadGroups);
    }
    public void indexDirect(long id,MemorySegment nativeIndices,int count,Object owner) {
        long bytes=count*4L;
        if(nativeIndices==null || !nativeIndices.isNative() || nativeIndices.byteSize()<bytes)
            throw new IllegalArgumentException("Direct terrain indices must be a native segment covering count*4 bytes");
        indexHeader(id,own(nativeIndices,bytes),count);
    }
    public void mesh(long id,byte[] vertices,int vertexCount,int layer) {
        if(vertexCount*28L!=vertices.length) throw new IllegalArgumentException("Vertex byte count mismatch");
        long offset=dataOffset(vertices.length);
        MemorySegment.copy(MemorySegment.ofArray(vertices),0,upload.segment,offset,vertices.length);
        var s=arrays[1].segment; long o=counts[1]++*(long)MESH_UPDATE_STRIDE;
        s.set(JAVA_LONG,o,id); s.set(ADDRESS,o+8,upload.segment.asSlice(offset));
        s.set(JAVA_INT,o+16,vertexCount); s.set(JAVA_INT,o+20,layer);
        s.set(ADDRESS,o+24,MemorySegment.NULL);
    }
    public void meshDirect(long id,MemorySegment nativeVertices,int vertexCount,int layer,Object owner,
            int[] groupQuadCounts,int layoutFlags,float[] minBounds,float[] maxBounds,byte[] wireframeGroups) {
        long bytes=vertexCount*28L;
        if(nativeVertices==null || !nativeVertices.isNative() || nativeVertices.byteSize()<bytes)
            throw new IllegalArgumentException("Direct terrain vertices must be a native segment covering vertexCount*28 bytes");
        meshRecord(id,own(nativeVertices,bytes),vertexCount,layer,groupQuadCounts,layoutFlags,minBounds,maxBounds,wireframeGroups);
    }
    private void meshRecord(long id,MemorySegment nativeVertices,int vertexCount,int layer,
            int[] groupQuadCounts,int layoutFlags,float[] minBounds,float[] maxBounds,byte[] wireframeGroups) {
        if(groupQuadCounts==null || groupQuadCounts.length!=7 || minBounds==null || minBounds.length!=3 || maxBounds==null || maxBounds.length!=3)
            throw new IllegalArgumentException("Invalid preanalyzed terrain layout arrays");
        MemorySegment wfPtr=MemorySegment.NULL;
        int wfLen=0;
        if(wireframeGroups!=null && wireframeGroups.length>0) {
            long wfOffset=dataOffset(wireframeGroups.length);
            MemorySegment.copy(MemorySegment.ofArray(wireframeGroups),0,upload.segment,wfOffset,wireframeGroups.length);
            wfPtr=upload.segment.asSlice(wfOffset,wireframeGroups.length);
            wfLen=wireframeGroups.length;
        }
        var ls=layouts.segment;
        long lo=layoutCount++*(long)MESH_LAYOUT_STRIDE;
        for(int g=0;g<7;g++) ls.set(JAVA_INT,lo+g*4L,groupQuadCounts[g]);
        ls.set(JAVA_INT,lo+28,layoutFlags);
        for(int a=0;a<3;a++) {
            ls.set(JAVA_FLOAT,lo+32+a*4L,minBounds[a]);
            ls.set(JAVA_FLOAT,lo+44+a*4L,maxBounds[a]);
        }
        ls.set(ADDRESS,lo+56,wfPtr);
        ls.set(JAVA_INT,lo+64,wfLen);
        ls.set(JAVA_INT,lo+68,0);

        var s=arrays[1].segment; long o=counts[1]++*(long)MESH_UPDATE_STRIDE;
        s.set(JAVA_LONG,o,id); s.set(ADDRESS,o+8,nativeVertices);
        s.set(JAVA_INT,o+16,vertexCount); s.set(JAVA_INT,o+20,layer);
        s.set(ADDRESS,o+24,ls.asSlice(lo,MESH_LAYOUT_STRIDE));
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
    public MemorySegment visibleSegment() { return arrays[3].segment; }
    public MemorySegment instanceSegment() { return arrays[4].segment; }
    public void setVisibleAndInstanceCounts(int visibleCount,int instanceCount) {
        if(visibleCount<0 || visibleCount*24L>arrays[3].segment.byteSize() ||
           instanceCount<0 || instanceCount*16L>arrays[4].segment.byteSize()) {
            throw new IllegalArgumentException("Visible or instance count exceeds reserved capacity");
        }
        counts[3]=visibleCount;
        counts[4]=instanceCount;
    }
    public int visibleCount() { return counts[3]; }
    private static void floats(MemorySegment target,long offset,float[] values,int count) {
        if(values.length!=count) throw new IllegalArgumentException("Expected "+count+" floats");
        for(int i=0;i<count;i++) target.set(JAVA_FLOAT,offset+i*4L,values[i]);
    }
    public MemorySegment finish(NativeRenderer.Frame frame) {
        var s=descriptor; s.set(JAVA_INT,0,BYTE_SIZE); s.set(JAVA_INT,4,frame.flags);
        s.set(JAVA_LONG,8,frame.epoch); s.set(JAVA_LONG,16,frame.outputSequence);
        for(int i=0;i<5;i++) {
            s.set(ADDRESS,24+i*8L,counts[i]==0?MemorySegment.NULL:arrays[i].segment);
            s.set(JAVA_INT,64+i*4L,counts[i]);
        }
        floats(s,96,frame.projection,16); floats(s,160,frame.view,16);
        for(int i=0;i<3;i++) s.set(JAVA_INT,224+i*4L,frame.cameraBlock[i]);
        floats(s,236,frame.cameraFraction,3); floats(s,248,frame.fogColor,4); floats(s,264,frame.fogDistances,4);
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
        return finish(frame);
    }
    @Override public void close() {
        if(closed) return;
        clearBorrowed();
        for(var storage:arrays) storage.close();
        layouts.close();
        upload.close();
        fixed.close();
        closed=true;
    }
}
