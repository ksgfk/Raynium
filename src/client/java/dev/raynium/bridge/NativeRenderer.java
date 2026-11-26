package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static java.lang.foreign.ValueLayout.*;

/** Owner-thread frontend for the renderer and its asynchronous submission stream. */
public final class NativeRenderer implements AutoCloseable {
    public static final int RGSS=1, IMPROVED_TRANSPARENCY=4, WIREFRAME=128, REUSE_VISIBLE=256, REUSE_INSTANCES=512, INSTANCE_POOL=1024;
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
    /** Owner-thread lease on one native submission result, including across reentrant callbacks. */
    public final class PendingSubmit implements AutoCloseable {
        private final MemorySegment result=MemorySegment.ofArray(new long[NativeBindings.SUBMIT_BYTES/Long.BYTES]);
        private boolean success,leased;
        private PendingSubmit() {}
        public long sequence() { return result.get(JAVA_LONG,0); }
        public boolean success() { return success; }
        public SubmitResult snapshot() { return new SubmitResult(sequence(),success,Progress.read(result,8)); }
        public void publish() { syncWorkerState(result,8); }
        @Override public void close() {
            if(!leased) return;
            leased=false;
            if(!isClosed()) freeSubmits.addFirst(this);
        }
    }
    public static class Frame {
        public long epoch=1, outputSequence;
        public int flags;
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
    private final ArrayDeque<InFlightBatch> inFlightBatches=new ArrayDeque<>(),freeBatches=new ArrayDeque<>();
    private final ArrayDeque<PendingSubmit> freeSubmits=new ArrayDeque<>();
    private long consumedSequence,submittedSequence;
    private boolean dispatching;
    private int terminalFlags;
    private static final class InFlightBatch {
        long sequence;
        Runnable release,fail;
        List<Runnable> submitted,completed,recyclers;
        BatchCallbacks pooled=new BatchCallbacks();
    }
    public static final class BatchCallbacks {
        public final ArrayList<Runnable> submitted=new ArrayList<>(),completed=new ArrayList<>(),recyclers=new ArrayList<>();
        public void clear() { submitted.clear(); completed.clear(); recyclers.clear(); }
    }

    private Runnable releaseAcquiredSurface;
    private final Set<NativeBuffer> buffers=new HashSet<>();
    private final List<NativeBuffer> writableBuffers=new ArrayList<>();
    private final Arena scratch=Arena.ofConfined();
    private final MemorySegment waitResult=scratch.allocate(NativeBindings.WAIT_BYTES,8),batchDesc=scratch.allocate(NativeBindings.BATCH_BYTES,8),submitResult=scratch.allocate(NativeBindings.SUBMIT_BYTES,8);
    private static final int TELEMETRY_CAPACITY=32;
    private final MemorySegment telemetry=scratch.allocate(NativeBindings.TELEMETRY_BYTES*TELEMETRY_CAPACITY,8),telemetryCount=scratch.allocate(JAVA_INT);
    final MemorySegment bufferDesc=scratch.allocate(24,8),bufferResult=scratch.allocate(16,8);
    private final MemorySegment captureInfo=scratch.allocate(32,8);
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
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(32,4);
            desc.set(JAVA_INT,0,32); desc.set(JAVA_INT,4,width); desc.set(JAVA_INT,8,height); desc.set(JAVA_INT,12,mips);
            desc.set(JAVA_INT,16,Objects.requireNonNull(format).ordinal()); desc.set(JAVA_INT,20,layers); desc.set(JAVA_INT,24,flags);
            var out=arena.allocate(JAVA_LONG);
            NativeBindings.call(bindings.createTexture,"create texture",handle,desc,out);
            return new NativeTexture(this,out.get(JAVA_LONG,0),width,height,mips,layers,format,flags,usage,label);
        }
    }
    void uploadTexture(MemorySegment descriptor) { checkOpen(); NativeBindings.call(bindings.uploadTexture,"upload texture",handle,descriptor); }
    void destroyTexture(long id) { owner.checkOpen(); NativeBindings.call(bindings.destroyTexture,"destroy texture",handle,id); }
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
        var info=captureInfo;
        info.set(JAVA_INT,0,32); info.set(JAVA_INT,28,0);
        NativeBindings.call(bindings.pollCapture,"query capture",handle,sequence,info,MemorySegment.NULL,0L);
        if(info.get(JAVA_INT,4)==0) return Optional.empty();
        int size=Math.toIntExact(info.get(JAVA_LONG,16));
        try(var arena=Arena.ofConfined()) {
            var pixels=arena.allocate(size,8);
            info.set(JAVA_INT,0,32); info.set(JAVA_INT,28,0);
            NativeBindings.call(bindings.pollCapture,"read capture",handle,sequence,info,pixels,(long)size);
            return Optional.of(new Capture(info.get(JAVA_INT,8),info.get(JAVA_INT,12),pixels.toArray(JAVA_BYTE)));
        }
    }
    public static long nextFrameId() { return GENERATIONS.incrementAndGet(); }
    public long nextEpoch() {
        checkOpen();
        return GENERATIONS.incrementAndGet();
    }
    public long registerPipeline(String program,int[] words,byte[] external) {
        checkOpen();
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(40,8); var out=arena.allocate(JAVA_LONG);
            desc.set(JAVA_INT,0,40); desc.set(JAVA_INT,4,words.length);
            if(external!=null) { desc.set(ADDRESS,24,arena.allocateFrom(JAVA_BYTE,external)); desc.set(JAVA_LONG,32,external.length); }
            desc.set(ADDRESS,8,arena.allocateFrom(program)); desc.set(ADDRESS,16,arena.allocateFrom(JAVA_INT,words));
            NativeBindings.call(bindings.registerPipeline,"register pipeline",handle,desc,out);
            return out.get(JAVA_LONG,0);
        }
    }
    public void destroyPipeline(long id) {
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
    void destroyBuffer(long id) {
        owner.checkThread();
        if(!isClosed()) NativeBindings.call(bindings.destroyBuffer,"destroy buffer",handle,id);
    }
    void trackWritable(NativeBuffer buffer) { writableBuffers.add(buffer); }
    public void trackBatch(long sequence,List<Runnable> submitted,List<Runnable> completed,List<Runnable> recyclers,Runnable release,Runnable fail) {
        enqueueBatch(sequence,submitted,completed,recyclers,release,fail);
    }
    /** Transfers these lists to the in-flight batch and returns empty lists for the next recording. */
    public BatchCallbacks trackBatch(long sequence,BatchCallbacks callbacks,Runnable release,Runnable fail) {
        var batch=enqueueBatch(sequence,callbacks.submitted,callbacks.completed,callbacks.recyclers,release,fail);
        var empty=batch.pooled;
        batch.pooled=callbacks;
        return empty;
    }
    private InFlightBatch enqueueBatch(long sequence,List<Runnable> submitted,List<Runnable> completed,List<Runnable> recyclers,Runnable release,Runnable fail) {
        var batch=freeBatches.pollFirst();
        if(batch==null) batch=new InFlightBatch();
        batch.sequence=sequence;
        batch.release=release; batch.fail=fail;
        batch.submitted=submitted; batch.completed=completed; batch.recyclers=recyclers;
        inFlightBatches.addLast(batch);
        return batch;
    }
    private static Throwable collectFailure(Throwable first,Throwable next) {
        if(first==null) return next;
        if(first!=next) first.addSuppressed(next);
        return first;
    }
    private static Throwable run(Runnable callback,Throwable failure) {
        try { callback.run(); } catch(Throwable next) { return collectFailure(failure,next); }
        return failure;
    }
    private static Throwable runAll(List<Runnable> callbacks,Throwable failure) {
        for(int i=0;i<callbacks.size();i++) failure=run(callbacks.get(i),failure);
        callbacks.clear();
        return failure;
    }
    public void syncWorkerState(Progress p) {
        syncWorkerState(p.consumed(),p.submitted(),p.completed(),p.presented(),p.generation(),p.recovery(),p.flags());
    }
    private void syncWorkerState(MemorySegment snapshot,long offset) {
        syncWorkerState(snapshot.get(JAVA_LONG,offset),snapshot.get(JAVA_LONG,offset+8),snapshot.get(JAVA_LONG,offset+16),
            snapshot.get(JAVA_LONG,offset+24),snapshot.get(JAVA_INT,offset+32),snapshot.get(JAVA_INT,offset+36),snapshot.get(JAVA_INT,offset+44));
    }
    private void syncWorkerState(long consumed,long submitted,long completed,long presented,int generation,int reportedRecovery,int flags) {
        if(poisoned) return;
        terminalFlags|=flags;
        terminalSettling=terminalFlags!=0;
        consumedSequence=Math.max(consumedSequence,consumed);
        submittedSequence=Math.max(submittedSequence,submitted);
        completedSequence=Math.max(completedSequence,completed);
        if(generation==surfaceGeneration && reportedRecovery!=0 && reportedRecovery!=recovery) {
            recovery=reportedRecovery;
            if(updateRecovery!=null) updateRecovery.accept(recovery);
        }
        if(presented>observedPresented) {
            dev.raynium.client.benchmark.BenchmarkSession.nativeFrames(presented-observedPresented);
            observedPresented=presented;
        }
        if(dispatching) return;
        dispatching=true;
        Throwable failure=null;
        try {
            // Callbacks may submit work or poll a newer snapshot. Restart at the head after dispatch.
            while(!inFlightBatches.isEmpty()) {
                InFlightBatch ready=null;
                for(var batch:inFlightBatches) {
                    if(batch.release!=null && (batch.sequence<=consumedSequence || terminalSettling) ||
                        batch.submitted!=null && batch.sequence<=submittedSequence ||
                        batch.sequence<=completedSequence || terminalSettling) { ready=batch; break; }
                }
                if(ready==null) break;
                if(ready.release!=null && (ready.sequence<=consumedSequence || terminalSettling)) {
                    var release=ready.release; ready.release=null;
                    failure=run(release,failure);
                }
                if(ready.submitted!=null && ready.sequence<=submittedSequence) {
                    failure=runAll(ready.submitted,failure);
                    ready.submitted=null;
                }
                if(ready.sequence<=completedSequence || terminalSettling) {
                    inFlightBatches.removeFirst();
                    if(ready.sequence<=completedSequence) failure=runAll(ready.completed,failure);
                    else failure=run(ready.fail,failure);
                    failure=runAll(ready.recyclers,failure);
                    ready.pooled.clear();
                    ready.submitted=ready.completed=ready.recyclers=null;
                    ready.fail=null;
                    freeBatches.addFirst(ready);
                }
            }
        } finally { dispatching=false; }
        if(terminalSettling) {
            poisoned=true;
            terminalSettling=false;
            if(releaseAcquiredSurface!=null) releaseAcquiredSurface.run();
            Throwable cause=(terminalFlags&1)!=0?new com.mojang.renderpearl.api.device.GpuDeviceLossException("Graphics device lost"):
                    new IllegalStateException("Renderer submission failed");
            failure=collectFailure(failure,cause);
        }
        if(failure instanceof Error e) throw e;
        if(failure instanceof RuntimeException e) throw e;
        if(failure!=null) throw new IllegalStateException("Completion callback failed",failure);
    }
    public void pollCompleted() { waitSubmit(0,0,false); }
    public long createQueryPool(int count) {
        checkOpen();
        if(count<=0) throw new IllegalArgumentException("Query pool must have a positive size");
        try(var arena=Arena.ofConfined()) {
            var id=arena.allocate(JAVA_LONG);
            NativeBindings.call(bindings.createQueryPool,"create query pool",handle,count,id);
            return id.get(JAVA_LONG,0);
        }
    }
    public void destroyQueryPool(long id) {
        checkThread();
        if(!isClosed()) NativeBindings.call(bindings.destroyQueryPool,"destroy query pool",handle,id);
    }
    public void syncQueryPool(long id,int first,int count,MemorySegment samples) {
        checkAlive();
        if(first<0 || count<0 || !samples.isNative() || samples.byteSize()<(long)count*16) throw new IllegalArgumentException("Invalid query result slice");
        try {
            var result=waitResult;
            int status=(int)bindings.syncQueryPool.invokeExact(handle,id,first,count,samples,result);
            if(status!=0) throw new IllegalStateException("Invalid query pool or range");
            syncWorkerState(result,0);
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
        return submitFrame(commands,uploads,instanceUploads,surface,views,measure,permit,MemorySegment.NULL,MemorySegment.NULL);
    }
    public SubmitResult submitFrame(MemorySegment commands,MemorySegment uploads,MemorySegment instanceUploads,NativeSwapchain surface,
            List<MemorySegment> views,boolean measure,dev.raynium.client.renderpearl.CommandTape.BorrowPermit permit,
            MemorySegment captures,MemorySegment queries) {
        try(var batch=enqueueFrame(commands,uploads,instanceUploads,surface,views,measure,permit,captures,queries)) {
            return batch.snapshot();
        }
    }
    public PendingSubmit enqueueFrame(MemorySegment commands,MemorySegment uploads,MemorySegment instanceUploads,NativeSwapchain surface,
            List<MemorySegment> views,boolean measure,dev.raynium.client.renderpearl.CommandTape.BorrowPermit permit,
            MemorySegment captures,MemorySegment queries) {
        checkOpen();
        if(surface!=null && surface!=swapchain) throw new IllegalArgumentException("Invalid submit surface");
        var batch=freeSubmits.pollFirst();
        if(batch==null) batch=new PendingSubmit();
        batch.leased=true;
        try {
            var desc=batchDesc;
            desc.set(JAVA_INT,0,NativeBindings.BATCH_BYTES); desc.set(JAVA_INT,4,(measure?1:0)|(permit!=null?2:0));
            var records=viewRecords.reserve(views.size());
            desc.set(ADDRESS,96,captures); desc.set(JAVA_INT,104,Math.toIntExact(captures.byteSize()/8));
            desc.set(ADDRESS,112,queries); desc.set(JAVA_INT,120,Math.toIntExact(queries.byteSize()/16));
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
            int status=(int)bindings.submitFrame.invokeExact(handle,desc,submitResult);
            batch.result.copyFrom(submitResult);
            batch.success=status==0;
            if(batch.sequence()>0) {
                if(measure) writableBuffers.forEach(NativeBuffer::measureWrites);
                writableBuffers.removeIf(NativeBuffer::afterSubmit);
            }
            return batch;
        } catch(RuntimeException | Error failure) { batch.close(); throw failure; }
        catch(Throwable failure) { batch.close(); throw new IllegalStateException("Cannot submit command tape",failure); }
    }
    public boolean awaitSubmission(long sequence,long timeoutNs) {
        checkOpen();
        if(completedSequence<sequence) waitSubmit(sequence,timeoutNs,false);
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
            int outcome=waitResult.get(JAVA_INT,40);
            syncWorkerState(waitResult,0);
            if(status!=0) throw new IllegalStateException("Renderer flush failed");
            return outcome;
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
        return waitSubmit(sequence,timeoutNs,true);
    }
    private WaitResult waitSubmit(long sequence,long timeoutNs,boolean returnSnapshot) {
        checkOpen();
        if(sequence<0) throw new IllegalArgumentException("Negative submission sequence");
        try {
            var result=waitResult;
            long start=dev.raynium.client.benchmark.BenchmarkSession.syncWaitStart();
            int status=(int)bindings.waitSubmit.invokeExact(handle,sequence,timeoutNs,result);
            var wait=returnSnapshot?new WaitResult(result.get(JAVA_LONG,16),status==0,(result.get(JAVA_INT,44)&1)!=0):null;
            if(start!=0) dev.raynium.client.benchmark.BenchmarkSession.syncWaitEnd(start);
            syncWorkerState(result,0);
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
            if(!isPoisoned() && !inFlightBatches.isEmpty()) flush();
        } finally {
            NativeBindings.call(bindings.destroyRenderer,"destroy renderer",handle);
            handle=MemorySegment.NULL;
            for(var buffer:buffers) buffer.endRenderer();
            buffers.clear(); writableBuffers.clear();
            viewRecords.close(); dirtyRanges.close(); scratch.close();
            freeBatches.clear(); freeSubmits.clear();
            owner.releasedRenderer(this);
        }
    }
}
