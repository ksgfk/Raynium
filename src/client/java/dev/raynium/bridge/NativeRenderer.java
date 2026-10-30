package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import static java.lang.foreign.ValueLayout.*;

/** Persistent scene owner. All frame arrays are borrowed only until render returns. */
public final class NativeRenderer implements AutoCloseable {
    public static final int UV=1, TEXTURED=2, PROCEDURAL=0, POSITION_COLOR_UV_LIGHT=1;
    public static final int DIRECT=1, RGSS=4, PROFILE=8;
    public record Index(long id,int[] values) {}
    public record Submesh(int first,int count,int baseVertex) {}
    public record Mesh(long id,byte[] vertices,long index,int vertexCount,int stride,int layout,List<Submesh> submeshes) {}
    public record Material(long id,int program,int albedo,int light,int anisotropy) {}
    public record Primitive(long id,long mesh,long material) {}
    public record Instance(int x,int y,int z,float visibility) {}
    public record Visible(long primitive,int submesh,int depthKey,int firstInstance,int instanceCount) {}
    public static final class Frame {
        public long epoch=1, outputSequence, inputSequence;
        public int flags,changedTextures;
        public List<Index> indices=List.of();
        public List<Mesh> meshes=List.of();
        public List<Material> materials=List.of();
        public List<Primitive> primitives=List.of();
        public List<Visible> visible=List.of();
        public List<Instance> instances=List.of();
        public long[] removedMeshes={},removedPrimitives={},removedMaterials={},removedIndices={};
        public float[] projection=identity(),view=identity(),cameraFraction=new float[3],fogColor=new float[4],fogDistances={1000,2000,1000,2000};
        public int[] cameraBlock=new int[3];
        private static float[] identity() { return new float[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
    }
    private final NativeRuntime owner;
    final NativeBindings bindings;
    private MemorySegment handle=MemorySegment.NULL;
    private long lastEpoch;
    NativeRenderTarget target;
    NativeSharedTextures textures;
    private NativeRenderer(NativeRuntime owner,NativeBindings bindings) { this.owner=owner; this.bindings=bindings; }
    static NativeRenderer create(NativeRuntime owner,NativeBindings bindings,MemorySegment runtime) {
        var renderer=new NativeRenderer(owner,bindings);
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(8,4); desc.set(JAVA_INT,0,8);
            var output=arena.allocate(ADDRESS);
            NativeBindings.call(bindings.createRenderer,"create renderer",runtime,desc,output);
            renderer.handle=output.get(ADDRESS,0); return renderer;
        }
    }
    public NativeRenderTarget createTarget(int width,int height) {
        checkOpen(); if(target!=null) throw new IllegalStateException("Renderer already has a target");
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(16,4); desc.set(JAVA_INT,0,16); desc.set(JAVA_INT,4,width); desc.set(JAVA_INT,8,height);
            var output=arena.allocate(ADDRESS); var info=arena.allocate(128,8);
            var result=new NativeRenderTarget(this);
            NativeBindings.call(bindings.createRenderTarget,"create render target",handle,desc,output,info,128);
            result.handle=output.get(ADDRESS,0); target=result;
            try { result.info=SharedImageInfo.read(info); return result; }
            catch(RuntimeException | Error failure) {
                try { result.close(); } catch(RuntimeException cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
        }
    }
    public NativeSharedTextures createSharedTextures(List<NativeSharedTextures.Texture> descriptions) {
        checkOpen(); if(textures!=null) throw new IllegalStateException("Renderer already has shared textures");
        if(descriptions.isEmpty() || descriptions.size()>16) throw new IllegalArgumentException("Expected 1..16 textures");
        try(var arena=Arena.ofConfined()) {
            var array=arena.allocate(descriptions.size()*16L,4); int count=0;
            for(int i=0;i<descriptions.size();i++) {
                var t=descriptions.get(i);
                if(t.width()<1 || t.height()<1 || t.mipLevels()<1 || t.mipLevels()>32) throw new IllegalArgumentException("Invalid texture extent");
                array.set(JAVA_INT,i*16L,t.width()); array.set(JAVA_INT,i*16L+4,t.height());
                array.set(JAVA_INT,i*16L+8,t.mipLevels()); array.set(JAVA_INT,i*16L+12,t.splitMips()?1:0);
                count+=t.splitMips()?t.mipLevels():1;
            }
            var desc=arena.allocate(16,8); desc.set(JAVA_INT,0,16); desc.set(JAVA_INT,4,descriptions.size()); desc.set(ADDRESS,8,array);
            var output=arena.allocate(ADDRESS); var info=arena.allocate(count*144L,8);
            var result=new NativeSharedTextures(this);
            NativeBindings.call(bindings.createSharedTextures,"create shared textures",handle,desc,output,info,count);
            result.handle=output.get(ADDRESS,0); textures=result;
            try {
                var exports=new ArrayList<NativeSharedTextures.Export>(count);
                for(int i=0;i<count;i++) {
                    var row=info.asSlice(i*144L,144);
                    exports.add(new NativeSharedTextures.Export(SharedImageInfo.read(row),row.get(JAVA_INT,128),row.get(JAVA_INT,132),row.get(JAVA_INT,136)));
                }
                result.exports=List.copyOf(exports); return result;
            } catch(RuntimeException | Error failure) {
                try { result.close(); } catch(RuntimeException cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
        }
    }
    public long nextEpoch() { checkOpen(); return ++lastEpoch; }
    public void render(NativeRenderTarget target,Frame frame) {
        checkOpen(); target.checkOpen();
        if(target.owner!=this) throw new IllegalArgumentException("Render target owner mismatch");
        try(var arena=Arena.ofConfined()) {
            NativeBindings.call(bindings.renderFrame,"render frame",handle,target.handle,marshal(arena,frame));
            lastEpoch=Math.max(lastEpoch,frame.epoch);
        }
    }
    private static MemorySegment array(Arena arena,int count,int stride) {
        return count==0?MemorySegment.NULL:arena.allocate((long)count*stride,8);
    }
    private static MemorySegment copy(Arena arena,MemorySegment source) {
        if(source.byteSize()==0) return MemorySegment.NULL;
        var result=arena.allocate(source.byteSize(),8); result.copyFrom(source); return result;
    }
    private static void floats(MemorySegment dest,long offset,float[] values,int count) {
        if(values.length!=count) throw new IllegalArgumentException("Expected "+count+" floats");
        MemorySegment.copy(MemorySegment.ofArray(values),0,dest,offset,count*4L);
    }
    static MemorySegment marshal(Arena a,Frame f) {
        var s=a.allocate(344,8); s.set(JAVA_INT,0,344); s.set(JAVA_INT,4,f.flags);
        s.set(JAVA_LONG,8,f.epoch); s.set(JAVA_LONG,16,f.outputSequence); s.set(JAVA_LONG,24,f.inputSequence);
        var indices=array(a,f.indices.size(),24);
        for(int i=0;i<f.indices.size();i++) {
            var x=f.indices.get(i); long o=i*24L;
            indices.set(JAVA_LONG,o,x.id()); indices.set(ADDRESS,o+8,copy(a,MemorySegment.ofArray(x.values()))); indices.set(JAVA_INT,o+16,x.values().length);
        }
        var meshes=array(a,f.meshes.size(),48);
        for(int i=0;i<f.meshes.size();i++) {
            var x=f.meshes.get(i); long o=i*48L;
            if((long)x.vertexCount()*x.stride()!=x.vertices().length) throw new IllegalArgumentException("Vertex byte count mismatch");
            var ranges=array(a,x.submeshes().size(),16);
            for(int j=0;j<x.submeshes().size();j++) { var r=x.submeshes().get(j); ranges.set(JAVA_INT,j*16L,r.first()); ranges.set(JAVA_INT,j*16L+4,r.count()); ranges.set(JAVA_INT,j*16L+8,r.baseVertex()); }
            meshes.set(JAVA_LONG,o,x.id()); meshes.set(ADDRESS,o+8,copy(a,MemorySegment.ofArray(x.vertices()))); meshes.set(JAVA_LONG,o+16,x.index()); meshes.set(ADDRESS,o+24,ranges);
            meshes.set(JAVA_INT,o+32,x.vertexCount()); meshes.set(JAVA_INT,o+36,x.stride()); meshes.set(JAVA_INT,o+40,x.layout()); meshes.set(JAVA_INT,o+44,x.submeshes().size());
        }
        var materials=array(a,f.materials.size(),24);
        for(int i=0;i<f.materials.size();i++) { var x=f.materials.get(i); long o=i*24L; materials.set(JAVA_LONG,o,x.id()); materials.set(JAVA_INT,o+8,x.program()); materials.set(JAVA_INT,o+12,x.albedo()); materials.set(JAVA_INT,o+16,x.light()); materials.set(JAVA_INT,o+20,x.anisotropy()); }
        var primitives=array(a,f.primitives.size(),24);
        for(int i=0;i<f.primitives.size();i++) { var x=f.primitives.get(i); long o=i*24L; primitives.set(JAVA_LONG,o,x.id()); primitives.set(JAVA_LONG,o+8,x.mesh()); primitives.set(JAVA_LONG,o+16,x.material()); }
        var visible=array(a,f.visible.size(),24);
        for(int i=0;i<f.visible.size();i++) { var x=f.visible.get(i); long o=i*24L; visible.set(JAVA_LONG,o,x.primitive()); visible.set(JAVA_INT,o+8,x.submesh()); visible.set(JAVA_INT,o+12,x.depthKey()); visible.set(JAVA_INT,o+16,x.firstInstance()); visible.set(JAVA_INT,o+20,x.instanceCount()); }
        var instances=array(a,f.instances.size(),16);
        for(int i=0;i<f.instances.size();i++) { var x=f.instances.get(i); long o=i*16L; instances.set(JAVA_INT,o,x.x()); instances.set(JAVA_INT,o+4,x.y()); instances.set(JAVA_INT,o+8,x.z()); instances.set(JAVA_FLOAT,o+12,x.visibility()); }
        var arrays=new MemorySegment[]{indices,meshes,materials,primitives,copy(a,MemorySegment.ofArray(f.removedMeshes)),copy(a,MemorySegment.ofArray(f.removedPrimitives)),copy(a,MemorySegment.ofArray(f.removedMaterials)),copy(a,MemorySegment.ofArray(f.removedIndices)),visible,instances};
        int[] counts={f.indices.size(),f.meshes.size(),f.materials.size(),f.primitives.size(),f.removedMeshes.length,f.removedPrimitives.length,f.removedMaterials.length,f.removedIndices.length,f.visible.size(),f.instances.size()};
        for(int i=0;i<10;i++) { s.set(ADDRESS,32+i*8L,arrays[i]); s.set(JAVA_INT,112+i*4L,counts[i]); }
        s.set(JAVA_INT,152,f.changedTextures); floats(s,160,f.projection,16); floats(s,224,f.view,16);
        if(f.cameraBlock.length!=3) throw new IllegalArgumentException("Expected three camera coordinates");
        MemorySegment.copy(MemorySegment.ofArray(f.cameraBlock),0,s,288,12);
        floats(s,300,f.cameraFraction,3); floats(s,312,f.fogColor,4); floats(s,328,f.fogDistances,4); return s;
    }
    void checkOpen() { owner.checkOpen(); if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Renderer is closed"); }
    void checkThread() { owner.checkThread(); }
    @Override public void close() {
        checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        if(target!=null || textures!=null) throw new IllegalStateException("Close target and shared textures before renderer");
        NativeBindings.call(bindings.destroyRenderer,"destroy renderer",handle); handle=MemorySegment.NULL; owner.releasedRenderer(this);
    }
}
