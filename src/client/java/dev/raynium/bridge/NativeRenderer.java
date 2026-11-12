package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static java.lang.foreign.ValueLayout.*;

/** Persistent terrain residency owner. All frame arrays are borrowed only until present returns. */
public final class NativeRenderer implements AutoCloseable {
    public static final int RGSS=1, PROFILE=2, IMPROVED_TRANSPARENCY=4, FLIP_Y=8;
    public record Index(long meshId,int[] values) {}
    public record Mesh(long id,byte[] vertices,int vertexCount,int layer) {}
    public record Instance(int x,int y,int z,float visibility) {}
    public record Visible(long mesh,int elementMask,int sortKey,int firstInstance,int instanceCount) {}
    public record GuiDraw(int pipeline,int texture,int samplerBits,int firstVertex,int vertexCount,int x,int y,int width,int height) {}
    public static final class Frame {
        public long epoch=1, outputSequence;
        public int flags;
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
        checkOpen();
        if(nextTextureId>0xffff_ffffL) throw new IllegalStateException("Texture ID space exhausted");
        int id=(int)nextTextureId++;
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(24,4); desc.set(JAVA_INT,0,24); desc.set(JAVA_INT,4,id); desc.set(JAVA_INT,8,width);
            desc.set(JAVA_INT,12,height); desc.set(JAVA_INT,16,mips); desc.set(JAVA_INT,20,java.util.Objects.requireNonNull(format).ordinal());
            NativeBindings.call(bindings.createTexture,"create texture",handle,desc);
        }
        return new NativeTexture(this,id,width,height,mips,format);
    }
    void uploadTexture(MemorySegment descriptor) { checkOpen(); NativeBindings.call(bindings.uploadTexture,"upload texture",handle,descriptor); }
    void destroyTexture(int id) { checkOpen(); NativeBindings.call(bindings.destroyTexture,"destroy texture",handle,id); }
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
