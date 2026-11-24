package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static java.lang.foreign.ValueLayout.*;

/** Persistent terrain residency owner. All frame arrays are borrowed only until present returns. */
public final class NativeRenderer implements AutoCloseable {
    public static final int RGSS=1, IMPROVED_TRANSPARENCY=4, WIREFRAME=128, REUSE_VISIBLE=256, REUSE_INSTANCES=512;
    public record Capture(int width,int height,byte[] rgba) {}
    public record SubmitResult(long sequence,int outcome,boolean success,boolean deviceLost,long[] measurements) {}
    public record WaitResult(long completedSequence,boolean success,boolean deviceLost) {}
    public record Index(long meshId,int[] values) {}
    public record Mesh(long id,byte[] vertices,int vertexCount,int layer) {}
    public record Instance(int x,int y,int z,float visibility) {}
    public record Visible(long mesh,int elementMask,int sortKey,int firstInstance,int instanceCount) {}
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
        private static float[] identity() { return new float[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
    }
    private static final AtomicLong GENERATIONS=new AtomicLong();
    private final NativeRuntime owner;
    final NativeBindings bindings;
    private MemorySegment handle=MemorySegment.NULL;
    private long completedSequence;
    private boolean poisoned;
    private Runnable releaseAcquiredSurface;
    private final Set<NativeBuffer> buffers=new HashSet<>();
    private final List<NativeBuffer> writableBuffers=new ArrayList<>();
    private final Arena scratch=Arena.ofConfined();
    private final MemorySegment waitResult=scratch.allocate(16,8),batchDesc=scratch.allocate(96,8),submitResult=scratch.allocate(160,8);
    final MemorySegment bufferDesc=scratch.allocate(24,8),bufferResult=scratch.allocate(16,8);
    private final Records viewRecords=new Records(NativeFrameBuffer.BYTE_SIZE),dirtyRanges=new Records(24);

    private static final class Records implements AutoCloseable {
        final int stride;
        Arena arena;
        MemorySegment memory;
        int capacity,lowUse;
        Records(int stride) { this.stride=stride; }
        MemorySegment reserve(int count) {
            if(count<=capacity/4 && capacity>4) ++lowUse; else lowUse=0;
            if(count>capacity || lowUse>=120) {
                close();
                lowUse=0;
                if(count==0) return MemorySegment.NULL;
                capacity=Math.max(4,Math.multiplyExact(Integer.highestOneBit(Math.max(count-1,1)),2));
                arena=Arena.ofConfined();
                memory=arena.allocate((long)capacity*stride,8);
            }
            return count==0?MemorySegment.NULL:memory;
        }
        public void close() {
            if(arena!=null) arena.close();
            arena=null; memory=null; capacity=0;
        }
    }
    private record Completion(long sequence,Runnable callback) {}
    private final ArrayDeque<Completion> completions=new ArrayDeque<>();
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
        } catch(RuntimeException | Error failure) { renderer.scratch.close(); throw failure; }
    }
    public NativeSwapchain createSwapchain(long hwnd,int width,int height,NativeSwapchain.PresentMode mode) {
        checkOpen(); if(swapchain!=null) throw new IllegalStateException("Renderer already has a swapchain");
        var value=new NativeSwapchain(this,hwnd);
        try(var arena=Arena.ofConfined()) {
            var output=arena.allocate(ADDRESS);
            NativeBindings.call(bindings.createSwapchain,"create swapchain",handle,NativeSwapchain.descriptor(arena,hwnd,width,height,mode),output);
            value.handle=output.get(ADDRESS,0); swapchain=value; return value;
        }
    }
    public NativeTexture createTexture(int width,int height,int mips,NativeTexture.Format format) {
        return createTexture(width,height,mips,1,format);
    }
    public NativeTexture createTexture(int width,int height,int mips,int layers,NativeTexture.Format format) {
        return createTexture(width,height,mips,layers,format,0);
    }
    public NativeTexture createTexture(int width,int height,int mips,int layers,NativeTexture.Format format,int flags) {
        int usage=NativeTexture.USAGE_COPY_DST|NativeTexture.USAGE_TEXTURE_BINDING;
        if((flags&NativeTexture.ATTACHMENT)!=0) usage|=NativeTexture.USAGE_RENDER_ATTACHMENT;
        if((flags&NativeTexture.READBACK)!=0) usage|=NativeTexture.USAGE_COPY_SRC;
        if((flags&NativeTexture.CUBE)!=0) usage|=NativeTexture.USAGE_CUBEMAP_COMPATIBLE;
        return createTexture(width,height,mips,layers,format,flags,usage,"");
    }
    public NativeTexture createTexture(String label,int usage,com.mojang.renderpearl.api.GpuFormat format,int width,int height,int layers,int mips) {
        if(usage==0 || (usage&~31)!=0) throw new IllegalArgumentException("Invalid texture usage");
        int flags=((usage&NativeTexture.USAGE_RENDER_ATTACHMENT)!=0?NativeTexture.ATTACHMENT:0) |
            ((usage&NativeTexture.USAGE_COPY_SRC)!=0?NativeTexture.READBACK:0) |
            ((usage&NativeTexture.USAGE_CUBEMAP_COMPATIBLE)!=0?NativeTexture.CUBE:0);
        return createTexture(width,height,mips,layers,NativeTexture.Format.from(format),flags,usage,label);
    }
    private NativeTexture createTexture(int width,int height,int mips,int layers,NativeTexture.Format format,int flags,int usage,String label) {
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
        return new NativeTexture(this,id,width,height,mips,layers,format,flags,usage,label);
    }
    void uploadTexture(MemorySegment descriptor) { checkOpen(); NativeBindings.call(bindings.uploadTexture,"upload texture",handle,descriptor); }
    void destroyTexture(int id) { owner.checkOpen(); NativeBindings.call(bindings.destroyTexture,"destroy texture",handle,id); }
    public int upsertModelMesh(int meshId,byte[] vertices,int vertexCount,int indexCount) {
        checkOpen();
        if(meshId<16 || vertices==null || vertices.length==0) throw new IllegalArgumentException("Invalid model mesh upsert");
        try(var arena=Arena.ofConfined()) {
            var nativeBytes=arena.allocateFrom(JAVA_BYTE,vertices);
            return upsertModelMesh(meshId,nativeBytes,vertexCount,indexCount);
        }
    }
    public int upsertModelMesh(int meshId,MemorySegment vertices,int vertexCount,int indexCount) {
        checkOpen();
        if(meshId<16 || vertices==null || !vertices.isNative() || vertices.byteSize()<=0 || vertices.byteSize()>Integer.MAX_VALUE)
            throw new IllegalArgumentException("Invalid native model mesh upsert");
        try(var arena=Arena.ofConfined()) {
            var page=arena.allocate(JAVA_INT);
            NativeBindings.call(bindings.upsertModelMesh,"upsert model mesh",handle,meshId,vertices,(int)vertices.byteSize(),vertexCount,indexCount,page);
            return page.get(JAVA_INT,0);
        }
    }
    public void removeModelMesh(int meshId) {
        checkThread();
        if(meshId<16) throw new IllegalArgumentException("Invalid model mesh id");
        if(!isClosed()) NativeBindings.call(bindings.removeModelMesh,"remove model mesh",handle,meshId);
    }
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
    public static long nextFrameId() { return GENERATIONS.incrementAndGet(); }
    public long nextEpoch() {
        checkOpen();
        return GENERATIONS.incrementAndGet();
    }
    public int registerPipeline(String program,int[] words,byte[] external) {
        checkOpen();
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(40,8); var out=arena.allocate(JAVA_INT);
            desc.set(JAVA_INT,0,40); desc.set(JAVA_INT,4,words.length);
            if(external!=null) { desc.set(ADDRESS,24,arena.allocateFrom(JAVA_BYTE,external)); desc.set(JAVA_LONG,32,external.length); }
            desc.set(ADDRESS,8,arena.allocateFrom(program)); desc.set(ADDRESS,16,arena.allocateFrom(JAVA_INT,words));
            NativeBindings.call(bindings.registerPipeline,"register pipeline",handle,desc,out);
            return out.get(JAVA_INT,0);
        }
    }
    public void destroyPipeline(int id) {
        checkThread();
        if(!isClosed()) NativeBindings.call(bindings.destroyPipeline,"destroy pipeline",handle,id);
    }
    public NativeBuffer createBuffer(int usage,long size,java.nio.ByteBuffer initial,java.util.function.Supplier<dev.raynium.client.renderpearl.CommandTape> currentBatch) {
        checkOpen();
        var buffer=NativeBuffer.create(this,handle,usage,size,initial,currentBatch);
        buffers.add(buffer);
        return buffer;
    }
    void bufferReleased(NativeBuffer buffer) { buffers.remove(buffer); }
    void destroyBuffer(int id) {
        owner.checkThread();
        if(!isClosed()) NativeBindings.call(bindings.destroyBuffer,"destroy buffer",handle,id);
    }
    void trackWritable(NativeBuffer buffer) { writableBuffers.add(buffer); }
    public void onCompleted(long sequence,List<Runnable> callbacks) {
        for(var callback:callbacks) completions.addLast(new Completion(sequence,callback));
    }
    private void publishCompleted(long sequence) {
        completedSequence=Math.max(completedSequence,sequence);
        while(!completions.isEmpty() && completions.getFirst().sequence()<=completedSequence)
            completions.removeFirst().callback().run();
    }
    public void pollCompleted() {
        var result=waitSubmit(0,0);
        if(!result.success()) terminalFailure(result.deviceLost());
        publishCompleted(result.completedSequence());
    }
    public int createQueryPool(int count) {
        checkOpen();
        if(count<=0) throw new IllegalArgumentException("Query pool must have a positive size");
        try(var arena=Arena.ofConfined()) {
            var id=arena.allocate(JAVA_INT);
            NativeBindings.call(bindings.createQueryPool,"create query pool",handle,count,id);
            return id.get(JAVA_INT,0);
        }
    }
    public void destroyQueryPool(int id) {
        checkThread();
        if(!isClosed()) NativeBindings.call(bindings.destroyQueryPool,"destroy query pool",handle,id);
    }
    public void syncQueryPool(int id,int first,int count,MemorySegment samples) {
        checkOpen();
        if(first<0 || count<0 || !samples.isNative() || samples.byteSize()<(long)count*16) throw new IllegalArgumentException("Invalid query result slice");
        try {
            var result=waitResult;
            int status=(int)bindings.syncQueryPool.invokeExact(handle,id,first,count,samples,result);
            long completed=result.get(JAVA_LONG,0);
            boolean lost=(result.get(JAVA_INT,8)&1)!=0;
            if(status!=0) terminalFailure(lost);
            publishCompleted(completed);
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("Cannot sync query pool",failure); }
    }
    public SubmitResult submitFrame(MemorySegment commands,MemorySegment uploads,NativeSwapchain surface) {
        checkOpen();
        return submitFrame(commands,uploads,MemorySegment.NULL,surface,List.of(),false);
    }
    public SubmitResult submitFrame(MemorySegment commands,MemorySegment uploads,MemorySegment instanceUploads,NativeSwapchain surface,List<MemorySegment> views,boolean measure) {
        checkOpen();
        if(surface!=null && surface!=swapchain) throw new IllegalArgumentException("Invalid submit surface");
        try {
            var desc=batchDesc;
            desc.set(JAVA_INT,0,96); desc.set(JAVA_INT,4,measure?1:0);
            var records=viewRecords.reserve(views.size());
            desc.set(ADDRESS,80,records); desc.set(JAVA_INT,88,views.size());
            if(!views.isEmpty()) {
                for(int i=0;i<views.size();++i) records.asSlice((long)NativeFrameBuffer.BYTE_SIZE*i,NativeFrameBuffer.BYTE_SIZE).copyFrom(views.get(i));
            }
            desc.set(ADDRESS,8,surface==null?MemorySegment.NULL:surface.handle);
            desc.set(ADDRESS,16,commands);
            desc.set(JAVA_LONG,24,commands.byteSize());
            desc.set(ADDRESS,32,uploads);
            desc.set(JAVA_LONG,40,uploads.byteSize());
            desc.set(ADDRESS,48,instanceUploads); desc.set(JAVA_LONG,56,instanceUploads.byteSize());
            int count=0;
            for(int i=0;i<writableBuffers.size();i++) count=Math.addExact(count,writableBuffers.get(i).rangeCount());
            var dirty=dirtyRanges.reserve(count);
            long at=0;
            for(int i=0;i<writableBuffers.size();i++) at=writableBuffers.get(i).encodeRanges(dirty,at);
            desc.set(ADDRESS,64,dirty); desc.set(JAVA_INT,72,count);
            var result=submitResult;
            int status=(int)bindings.submitFrame.invokeExact(handle,desc,result);
            var batch=new SubmitResult(result.get(JAVA_LONG,0),result.get(JAVA_INT,8),status==0,(result.get(JAVA_INT,12)&1)!=0,measure?result.asSlice(16,144).toArray(JAVA_LONG):null);
            writableBuffers.removeIf(NativeBuffer::afterSubmit);
            return batch;
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("Cannot submit command tape",failure); }
    }
    public boolean awaitSubmission(long sequence,long timeoutNs) {
        checkOpen();
        if(completedSequence<sequence) {
            var wait=waitSubmit(sequence,timeoutNs);
            if(!wait.success()) terminalFailure(wait.deviceLost());
            publishCompleted(wait.completedSequence());
        }
        return completedSequence>=sequence;
    }
    public void onTerminalSurface(Runnable reset) { releaseAcquiredSurface=reset; }
    public void terminalFailure(boolean deviceLost) {
        poisoned=true;
        if(releaseAcquiredSurface!=null) releaseAcquiredSurface.run();
        if(deviceLost) throw new com.mojang.renderpearl.api.device.GpuDeviceLossException("Graphics device lost");
        throw new IllegalStateException("Renderer submission failed");
    }
    public WaitResult waitSubmit(long sequence,long timeoutNs) {
        checkOpen();
        if(sequence<0) throw new IllegalArgumentException("Negative submission sequence");
        try {
            var result=waitResult;
            long start=dev.raynium.client.benchmark.BenchmarkSession.syncWaitStart();
            int status=(int)bindings.waitSubmit.invokeExact(handle,sequence,timeoutNs,result);
            var wait=new WaitResult(result.get(JAVA_LONG,0),status==0,(result.get(JAVA_INT,8)&1)!=0);
            if(start!=0) dev.raynium.client.benchmark.BenchmarkSession.syncWaitEnd(start);
            return wait;
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("Cannot wait for renderer submission",failure); }
    }
    public void checkOpen() { owner.checkOpen(); if(poisoned) throw new IllegalStateException("Renderer in terminal failure"); if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Renderer is closed"); }
    void checkThread() { owner.checkThread(); }
    boolean isClosed() { return handle.equals(MemorySegment.NULL); }
    @Override public void close() {
        checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        if(swapchain!=null) throw new IllegalStateException("Close outputs before renderer");
        for(var buffer:buffers) buffer.endRenderer();
        buffers.clear();
        writableBuffers.clear();
        viewRecords.close(); dirtyRanges.close(); scratch.close();
        completions.clear();
        NativeBindings.call(bindings.destroyRenderer,"destroy renderer",handle); handle=MemorySegment.NULL; owner.releasedRenderer(this);
    }
}
