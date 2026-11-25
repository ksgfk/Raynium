package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static java.lang.foreign.ValueLayout.*;

/** Owner-thread frontend for the renderer and its asynchronous submission stream. */
public final class NativeRenderer implements AutoCloseable {
    public static final int RGSS=1, IMPROVED_TRANSPARENCY=4, WIREFRAME=128, REUSE_VISIBLE=256, REUSE_INSTANCES=512;
    public record Capture(int width,int height,byte[] rgba) {}
    public record Progress(long consumed,long submitted,long completed,long presented,int generation,int recovery,int outcome,int flags) {
        static Progress read(MemorySegment memory,long offset) {
            return new Progress(memory.get(JAVA_LONG,offset),memory.get(JAVA_LONG,offset+8),memory.get(JAVA_LONG,offset+16),
                    memory.get(JAVA_LONG,offset+24),memory.get(JAVA_INT,offset+32),memory.get(JAVA_INT,offset+36),
                    memory.get(JAVA_INT,offset+40),memory.get(JAVA_INT,offset+44));
        }
    }
    public record SubmitResult(long sequence,boolean success,Progress progress) {
        public boolean deviceLost() { return (progress.flags()&1)!=0; }
        public int outcome() { return progress.outcome(); }
    }
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
    private boolean poisoned,terminalSettling;
    private long observedPresented;
    private int surfaceGeneration,recovery;
    private java.util.function.IntConsumer updateRecovery;
    private final ArrayDeque<Completion> consumedCallbacks=new ArrayDeque<>(), submittedCallbacks=new ArrayDeque<>(), pendingBatches=new ArrayDeque<>();

    private Runnable releaseAcquiredSurface;
    private final Set<NativeBuffer> buffers=new HashSet<>();
    private final List<NativeBuffer> writableBuffers=new ArrayList<>();
    private final Arena scratch=Arena.ofConfined();
    private final MemorySegment waitResult=scratch.allocate(NativeBindings.WAIT_BYTES,8),batchDesc=scratch.allocate(96,8),submitResult=scratch.allocate(NativeBindings.SUBMIT_BYTES,8);
    private static final int TELEMETRY_CAPACITY=32;
    private final MemorySegment telemetry=scratch.allocate(NativeBindings.TELEMETRY_BYTES*TELEMETRY_CAPACITY,8),telemetryCount=scratch.allocate(JAVA_INT);
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
            value.handle=output.get(ADDRESS,0); swapchain=value; configuredSurface(); return value;
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
        checkAlive();
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
        if((usage&~1023)!=0) throw new IllegalArgumentException("Invalid public buffer usage");
        return allocateBuffer(usage,size,initial,currentBatch,NativeBuffer.WriteSource.HOST_SHADOW);
    }
    public NativeBuffer createFenceRotatedBuffer(int usage,long size,java.util.function.Supplier<dev.raynium.client.renderpearl.CommandTape> batch,NativeBuffer.WriteSource source) {
        checkOpen();
        if((usage&~1023)!=0) throw new IllegalArgumentException("Invalid rotating buffer usage");
        if((usage&(NativeBuffer.USAGE_MAP_WRITE|NativeBuffer.USAGE_COPY_DST))==NativeBuffer.USAGE_MAP_WRITE) usage|=1024;
        return allocateBuffer(usage,size,null,batch,source);
    }
    private NativeBuffer allocateBuffer(int usage,long size,java.nio.ByteBuffer initial,java.util.function.Supplier<dev.raynium.client.renderpearl.CommandTape> currentBatch,NativeBuffer.WriteSource source) {
        var buffer=NativeBuffer.create(this,handle,usage,size,initial,currentBatch,source);
        buffers.add(buffer);
        return buffer;
    }
    void bufferReleased(NativeBuffer buffer) { buffers.remove(buffer); }
    void destroyBuffer(int id) {
        owner.checkThread();
        if(!isClosed()) NativeBindings.call(bindings.destroyBuffer,"destroy buffer",handle,id);
    }
    void trackWritable(NativeBuffer buffer) { writableBuffers.add(buffer); }
    public void trackBatch(long sequence,List<Runnable> submitted,List<Runnable> completed,Runnable release,Runnable fail) {
        consumedCallbacks.addLast(new Completion(sequence,release));
        for(var callback:submitted) submittedCallbacks.addLast(new Completion(sequence,callback));
        for(var callback:completed) completions.addLast(new Completion(sequence,callback));
        pendingBatches.addLast(new Completion(sequence,fail));
    }
    private static Throwable collectFailure(Throwable first,Throwable next) {
        if(first==null) return next;
        if(first!=next) first.addSuppressed(next);
        return first;
    }
    private static Throwable dispatch(ArrayDeque<Completion> queue,long through,Throwable failure) {
        while(!queue.isEmpty() && queue.getFirst().sequence()<=through) {
            var callback=queue.removeFirst().callback();
            try { callback.run(); } catch(Throwable next) { failure=collectFailure(failure,next); }
        }
        return failure;
    }
    public void syncWorkerState(Progress p) {
        if(terminalSettling || poisoned) return;
        boolean terminal=p.flags()!=0;
        terminalSettling=terminal;
        completedSequence=Math.max(completedSequence,p.completed());
        if(p.generation()==surfaceGeneration && p.recovery()!=0 && p.recovery()!=recovery) {
            recovery=p.recovery();
            if(updateRecovery!=null) updateRecovery.accept(recovery);
        }
        if(p.presented()>observedPresented) {
            dev.raynium.client.benchmark.BenchmarkSession.nativeFrames(p.presented()-observedPresented);
            observedPresented=p.presented();
        }
        Throwable failure=dispatch(consumedCallbacks,p.consumed(),null);
        failure=dispatch(submittedCallbacks,p.submitted(),failure);
        failure=dispatch(completions,p.completed(),failure);
        while(!pendingBatches.isEmpty() && pendingBatches.getFirst().sequence()<=completedSequence) pendingBatches.removeFirst();
        if(terminal) {
            poisoned=true;
            terminalSettling=false;
            failure=dispatch(pendingBatches,Long.MAX_VALUE,failure);
            failure=dispatch(consumedCallbacks,Long.MAX_VALUE,failure);
            completions.clear(); submittedCallbacks.clear();
            if(releaseAcquiredSurface!=null) releaseAcquiredSurface.run();
            Throwable cause=(p.flags()&1)!=0?new com.mojang.renderpearl.api.device.GpuDeviceLossException("Graphics device lost"):
                    new IllegalStateException("Renderer submission failed");
            failure=collectFailure(failure,cause);
        }
        if(failure instanceof Error e) throw e;
        if(failure instanceof RuntimeException e) throw e;
        if(failure!=null) throw new IllegalStateException("Completion callback failed",failure);
    }
    public void pollCompleted() { waitSubmit(0,0); }
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
        checkAlive();
        if(first<0 || count<0 || !samples.isNative() || samples.byteSize()<(long)count*16) throw new IllegalArgumentException("Invalid query result slice");
        try {
            var result=waitResult;
            int status=(int)bindings.syncQueryPool.invokeExact(handle,id,first,count,samples,result);
            if(status!=0) throw new IllegalStateException("Invalid query pool or range");
            syncWorkerState(Progress.read(result,0));
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("Cannot sync query pool",failure); }
    }
    public SubmitResult submitFrame(MemorySegment commands,MemorySegment uploads,NativeSwapchain surface) {
        return submitFrame(commands,uploads,MemorySegment.NULL,surface,List.of(),false);
    }
    public SubmitResult submitFrame(MemorySegment commands,MemorySegment uploads,MemorySegment instanceUploads,NativeSwapchain surface,List<MemorySegment> views,boolean measure) {
        var result=submitFrame(commands,uploads,instanceUploads,surface,views,measure,null);
        syncWorkerState(result.progress());
        return result;
    }
    public SubmitResult submitFrame(MemorySegment commands,MemorySegment uploads,MemorySegment instanceUploads,NativeSwapchain surface,List<MemorySegment> views,boolean measure,dev.raynium.client.renderpearl.CommandTape.BorrowPermit permit) {
        checkOpen();
        if(surface!=null && surface!=swapchain) throw new IllegalArgumentException("Invalid submit surface");
        try {
            var desc=batchDesc;
            desc.set(JAVA_INT,0,96); desc.set(JAVA_INT,4,(measure?1:0)|(permit!=null?2:0));
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
            var batch=new SubmitResult(result.get(JAVA_LONG,0),status==0,Progress.read(result,8));
            if(batch.sequence()>0) {
                if(measure) writableBuffers.forEach(NativeBuffer::measureWrites);
                writableBuffers.removeIf(NativeBuffer::afterSubmit);
            }
            return batch;
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("Cannot submit command tape",failure); }
    }
    public boolean awaitSubmission(long sequence,long timeoutNs) {
        checkOpen();
        if(completedSequence<sequence) waitSubmit(sequence,timeoutNs);
        return completedSequence>=sequence;
    }
    public void onSurface(Runnable reset,java.util.function.IntConsumer receiver) {
        releaseAcquiredSurface=reset; updateRecovery=receiver;
    }
    void configuredSurface() { ++surfaceGeneration; recovery=0; }
    public int flush() {
        checkOpen();
        try {
            int status=(int)bindings.flushRenderer.invokeExact(handle,waitResult);
            var progress=Progress.read(waitResult,0);
            syncWorkerState(progress);
            if(status!=0) throw new IllegalStateException("Renderer flush failed");
            return progress.outcome();
        } catch(RuntimeException | Error e) { throw e; }
        catch(Throwable e) { throw new IllegalStateException("Cannot flush renderer",e); }
    }
    public void drainTelemetry(java.util.function.Consumer<MemorySegment> consumer) {
        checkAlive();
        int count;
        do {
            NativeBindings.call(bindings.drainTelemetry,"drain telemetry",handle,telemetry,TELEMETRY_CAPACITY,telemetryCount);
            count=telemetryCount.get(JAVA_INT,0);
            for(int i=0;i<count;i++) consumer.accept(telemetry.asSlice((long)i*NativeBindings.TELEMETRY_BYTES,NativeBindings.TELEMETRY_BYTES));
        } while(count==TELEMETRY_CAPACITY);
    }
    public WaitResult waitSubmit(long sequence,long timeoutNs) {
        checkOpen();
        if(sequence<0) throw new IllegalArgumentException("Negative submission sequence");
        try {
            var result=waitResult;
            long start=dev.raynium.client.benchmark.BenchmarkSession.syncWaitStart();
            int status=(int)bindings.waitSubmit.invokeExact(handle,sequence,timeoutNs,result);
            var p=Progress.read(result,0);
            var wait=new WaitResult(p.completed(),status==0,(p.flags()&1)!=0);
            if(start!=0) dev.raynium.client.benchmark.BenchmarkSession.syncWaitEnd(start);
            syncWorkerState(p);
            return wait;
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("Cannot wait for renderer submission",failure); }
    }
    public boolean isPoisoned() { return poisoned || terminalSettling; }
    public void checkAlive() { owner.checkOpen(); if(isClosed()) throw new IllegalStateException("Renderer is closed"); }
    public void checkOpen() { checkAlive(); if(isPoisoned()) throw new IllegalStateException("Renderer in terminal failure"); }
    void checkThread() { owner.checkThread(); }
    public boolean isClosed() { return handle.equals(MemorySegment.NULL); }
    @Override public void close() {
        checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        if(swapchain!=null) throw new IllegalStateException("Close outputs before renderer");
        try {
            if(!isPoisoned() && !pendingBatches.isEmpty()) flush();
        } finally {
            NativeBindings.call(bindings.destroyRenderer,"destroy renderer",handle);
            handle=MemorySegment.NULL;
            for(var buffer:buffers) buffer.endRenderer();
            buffers.clear(); writableBuffers.clear();
            viewRecords.close(); dirtyRanges.close(); scratch.close();
            consumedCallbacks.clear(); submittedCallbacks.clear(); completions.clear(); pendingBatches.clear();
            owner.releasedRenderer(this);
        }
    }
}
