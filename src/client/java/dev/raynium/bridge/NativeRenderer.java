package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static java.lang.foreign.ValueLayout.*;

/** Persistent terrain residency owner. All frame arrays are borrowed only until present returns. */
public final class NativeRenderer implements AutoCloseable {
    public static final int RGSS=1, PROFILE=2, IMPROVED_TRANSPARENCY=4, FLIP_Y=8, CAPTURE=16, CAPTURE_PREVIOUS=32,WORLD_ONLY=64,WIREFRAME=128,MEASURE=256;
    public record Capture(int width,int height,byte[] rgba) {}
    public static final int CAPTURE_FINAL=0,CAPTURE_LAST=1,CAPTURE_WORLD=2,CAPTURE_TEXTURE=3,CAPTURE_TRACY=4;
    public record Index(long meshId,int[] values) {}
    public record Mesh(long id,byte[] vertices,int vertexCount,int layer) {}
    public record Instance(int x,int y,int z,float visibility) {}
    public record Visible(long mesh,int elementMask,int sortKey,int firstInstance,int instanceCount) {}
    public record GuiDraw(int pipeline,int texture,int samplerBits,int firstVertex,int vertexCount,int x,int y,int width,int height) {}
    public static final class Frame {
        public long epoch=1, outputSequence;
        public int flags;
        public int renderWidth,renderHeight;
        public List<Index> indices=List.of();
        public List<Mesh> meshes=List.of();
        public List<Visible> visible=List.of();
        public List<Instance> instances=List.of();
        public long[] removedMeshes={};
        public int terrainAlbedoTexture,terrainLightTexture,terrainAnisotropy=1;
        public float[] projection=identity(),view=identity(),cameraFraction=new float[3],fogColor=new float[4],fogDistances={1000,2000,1000,2000};
        public int[] cameraBlock=new int[3];
        public byte[] guiVertices={};
        public List<GuiDraw> guiDraws=List.of();
        public float guiWidth,guiHeight;
        public float[] clearColor;
        private static float[] identity() { return new float[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
    }
    private static final AtomicLong GENERATIONS=new AtomicLong();
    private final NativeFrameBuffer frames=new NativeFrameBuffer();
    private final NativeRuntime owner;
    final NativeBindings bindings;
    private MemorySegment handle=MemorySegment.NULL;
    private long lastEpoch;
    private long nextTextureId=16;
    private int nextSceneBufferId=1;
    NativeSwapchain swapchain;
    private NativeRenderer(NativeRuntime owner,NativeBindings bindings) { this.owner=owner; this.bindings=bindings; }
    static NativeRenderer create(NativeRuntime owner,NativeBindings bindings,MemorySegment runtime,NativeShaderPack shaders,TerrainSubmission submission) {
        var renderer=new NativeRenderer(owner,bindings);
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(24,8); desc.set(JAVA_INT,0,24);
            desc.set(JAVA_INT,4,submission.flags());
            desc.set(ADDRESS,8,shaders.copyTo(arena)); desc.set(JAVA_LONG,16,shaders.size());
            var output=arena.allocate(ADDRESS);
            NativeBindings.call(bindings.createRenderer,"create renderer",runtime,desc,output);
            renderer.handle=output.get(ADDRESS,0); return renderer;
        } catch(RuntimeException | Error failure) { renderer.frames.close(); throw failure; }
    }
    public NativeSwapchain createSwapchain(long hwnd,int width,int height,NativeSwapchain.PresentMode mode) {
        checkOpen(); if(swapchain!=null) throw new IllegalStateException("Renderer already has a swapchain");
        var value=new NativeSwapchain(this,hwnd);
        try(var arena=Arena.ofConfined()) {
            var output=arena.allocate(ADDRESS);
            NativeBindings.call(bindings.createSwapchain,"create swapchain",handle,NativeSwapchain.descriptor(arena,hwnd,width,height,mode),output);
            value.handle=output.get(ADDRESS,0); swapchain=value; return value;
        } catch(RuntimeException | Error failure) { value.closeScratch(); throw failure; }
    }
    public NativeTexture createTexture(int width,int height,int mips,NativeTexture.Format format) {
        return createTexture(width,height,mips,1,format);
    }
    public NativeTexture createTexture(int width,int height,int mips,int layers,NativeTexture.Format format) {
        return createTexture(width,height,mips,layers,format,0);
    }
    public NativeTexture createTexture(int width,int height,int mips,int layers,NativeTexture.Format format,int flags) {
        checkOpen();
        if(nextTextureId>0xffff_ffffL) throw new IllegalStateException("Texture ID space exhausted");
        int id=(int)nextTextureId++;
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(32,4); desc.set(JAVA_INT,0,32); desc.set(JAVA_INT,4,id); desc.set(JAVA_INT,8,width);
            desc.set(JAVA_INT,12,height); desc.set(JAVA_INT,16,mips); desc.set(JAVA_INT,20,java.util.Objects.requireNonNull(format).ordinal());
            desc.set(JAVA_INT,24,layers);
            desc.set(JAVA_INT,28,flags);
            NativeBindings.call(bindings.createTexture,"create texture",handle,desc);
        }
        return new NativeTexture(this,id,width,height,mips,layers,format);
    }
    void uploadTexture(MemorySegment descriptor) { checkOpen(); NativeBindings.call(bindings.uploadTexture,"upload texture",handle,descriptor); }
    public NativeSceneBuffer createSceneBuffer(long size) {
        checkOpen();
        if(size<=0 || size>(1L<<30)) throw new IllegalArgumentException("Scene buffer size must be in [1, 1 GiB]");
        if(nextSceneBufferId==Integer.MAX_VALUE) throw new IllegalStateException("Scene buffer ID space exhausted");
        int id=nextSceneBufferId++;
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(16,8); desc.set(JAVA_INT,0,16); desc.set(JAVA_INT,4,id); desc.set(JAVA_LONG,8,size);
            NativeBindings.call(bindings.createSceneBuffer,"create scene buffer",handle,desc);
        }
        return new NativeSceneBuffer(this,id,size);
    }
    void uploadSceneBuffer(MemorySegment desc) { checkOpen(); NativeBindings.call(bindings.uploadSceneBuffer,"upload scene buffer",handle,desc); }
    void destroySceneBuffer(int id) { checkOpen(); NativeBindings.call(bindings.destroySceneBuffer,"destroy scene buffer",handle,id); }
    void destroyTexture(int id) { checkOpen(); NativeBindings.call(bindings.destroyTexture,"destroy texture",handle,id); }
    /** Empty while pending. Reading a completed capture consumes its output-sequence token. */
    public Optional<Capture> pollCapture(long sequence) {
        checkOpen();
        try(var arena=Arena.ofConfined()) {
            var info=arena.allocate(32,8); info.set(JAVA_INT,0,32);
            NativeBindings.call(bindings.pollCapture,"query capture",handle,sequence,info,MemorySegment.NULL,0L);
            if(info.get(JAVA_INT,4)==0) return Optional.empty();
            int size=Math.toIntExact(info.get(JAVA_LONG,16));
            var pixels=arena.allocate(size,8);
            NativeBindings.call(bindings.pollCapture,"read capture",handle,sequence,info,pixels,(long)size);
            return Optional.of(new Capture(info.get(JAVA_INT,8),info.get(JAVA_INT,12),pixels.toArray(JAVA_BYTE)));
        }
    }
    public void requestCapture(long id,int source,int texture,int mip,int layer) {
        checkOpen();
        try(var arena=Arena.ofConfined()) {
            var request=arena.allocate(32,8); request.set(JAVA_INT,0,32); request.set(JAVA_INT,4,source); request.set(JAVA_LONG,8,id);
            request.set(JAVA_INT,16,texture); request.set(JAVA_INT,20,mip); request.set(JAVA_INT,24,layer);
            NativeBindings.call(bindings.requestCapture,"request capture",handle,request);
        }
    }
    public static long nextFrameId() { return GENERATIONS.incrementAndGet(); }
    public long nextEpoch() {
        checkOpen();
        return GENERATIONS.updateAndGet(value->Math.max(value,lastEpoch)+1);
    }
    public NativeFrameBuffer frameBuffer() { checkOpen(); return frames; }
    public NativeSwapchain.Presentation present(NativeSwapchain target,Frame frame) {
        checkSwapchain(target); return present(target,frame,frames.marshal(frame));
    }
    public NativeSwapchain.Presentation presentPrepared(NativeSwapchain target,Frame frame) {
        checkSwapchain(target); return present(target,frame,frames.finish(frame));
    }
    private void checkSwapchain(NativeSwapchain target) {
        checkOpen(); target.checkOpen();
        if(target.owner!=this) throw new IllegalArgumentException("Swapchain owner mismatch");
    }
    private NativeSwapchain.Presentation present(NativeSwapchain target,Frame frame,MemorySegment descriptor) {
        NativeBindings.callPresentFrame(bindings.presentFrame,handle,target.handle,descriptor,target.result);
        lastEpoch=Math.max(lastEpoch,frame.epoch); return target.presentation();
    }
    void checkOpen() { owner.checkOpen(); if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Renderer is closed"); }
    void checkThread() { owner.checkThread(); }
    boolean isClosed() { return handle.equals(MemorySegment.NULL); }
    @Override public void close() {
        checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        if(swapchain!=null) throw new IllegalStateException("Close outputs before renderer");
        NativeBindings.call(bindings.destroyRenderer,"destroy renderer",handle); handle=MemorySegment.NULL; frames.close(); owner.releasedRenderer(this);
    }
}
