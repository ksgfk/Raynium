package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static java.lang.foreign.ValueLayout.*;

/** Persistent scene owner. All frame arrays are borrowed only until render returns. */
public final class NativeRenderer implements AutoCloseable {
    public static final int RGSS=1, PROFILE=2;
    public record Index(long id,int[] values) {}
    public record Submesh(int first,int count,int baseVertex) {}
    public record Mesh(long id,byte[] vertices,long index,int vertexCount,List<Submesh> submeshes) {}
    public record Material(long id,int albedo,int light,int anisotropy) {}
    public record Primitive(long id,long mesh,long material) {}
    public record Instance(int x,int y,int z,float visibility) {}
    public record Visible(long primitive,int elementMask,int sortKey,int firstInstance,int instanceCount) {}
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
    private static final AtomicLong GENERATIONS=new AtomicLong();
    private final NativeFrameBuffer frames=new NativeFrameBuffer();
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
        } catch(RuntimeException | Error failure) { renderer.frames.close(); throw failure; }
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
    public static long nextFrameId() { return GENERATIONS.incrementAndGet(); }
    public long nextEpoch() {
        checkOpen();
        return GENERATIONS.updateAndGet(value->Math.max(value,lastEpoch)+1);
    }
    public NativeFrameBuffer frameBuffer() { checkOpen(); return frames; }
    public void render(NativeRenderTarget target,Frame frame) {
        checkTarget(target);
        submit(target,frame,frames.marshal(frame));
    }
    /** Submits data written to frameBuffer since its most recent begin. */
    public void renderPrepared(NativeRenderTarget target,Frame parameters) {
        checkTarget(target);
        submit(target,parameters,frames.finish(parameters));
    }
    private void checkTarget(NativeRenderTarget target) {
        checkOpen(); target.checkOpen();
        if(target.owner!=this) throw new IllegalArgumentException("Render target owner mismatch");
    }
    private void submit(NativeRenderTarget target,Frame parameters,MemorySegment descriptor) {
        NativeBindings.callRenderFrame(bindings.renderFrame,handle,target.handle,descriptor,frames.error(),1024);
        lastEpoch=Math.max(lastEpoch,parameters.epoch);
    }
    void checkOpen() { owner.checkOpen(); if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Renderer is closed"); }
    void checkThread() { owner.checkThread(); }
    @Override public void close() {
        checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        if(target!=null || textures!=null) throw new IllegalStateException("Close target and shared textures before renderer");
        NativeBindings.call(bindings.destroyRenderer,"destroy renderer",handle); handle=MemorySegment.NULL; frames.close(); owner.releasedRenderer(this);
    }
}
