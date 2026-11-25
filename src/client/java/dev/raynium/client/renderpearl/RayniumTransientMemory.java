package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.backend.util.TransientBlockAllocator;
import com.mojang.renderpearl.backend.util.TransientBlockAllocator.Allocator;
import dev.raynium.bridge.NativeBuffer;
import dev.raynium.bridge.NativeRenderer;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.function.Supplier;
import org.lwjgl.system.MemoryUtil;

final class RayniumTransientMemory implements TransientMemory,AutoCloseable {
    private record Block(NativeBuffer buffer) implements Allocator.Block {
        @Override public boolean suboptimal() { return false; }
    }
    private final TransientBlockAllocator<Allocator.CpuBlock> cpu=new TransientBlockAllocator<>(524288,16,Allocator.CpuBlock.memalloc());
    private final HashMap<Integer,TransientBlockAllocator<Block>> gpu=new HashMap<>();
    private final NativeRenderer renderer;
    private final Supplier<CommandTape> batch;

    private final long hostCoherentAtomSize;

    RayniumTransientMemory(NativeRenderer renderer,Supplier<CommandTape> batch,long hostCoherentAtomSize) {
        if(hostCoherentAtomSize<1) throw new IllegalArgumentException("Invalid host coherent atom size");
        this.hostCoherentAtomSize=hostCoherentAtomSize;
        this.renderer=renderer; this.batch=batch;
    }
    private static void validate(long size,long alignment,long minimum,long element) {
        if(size<=0 || size>Integer.MAX_VALUE || alignment<=0 || minimum<=0 || minimum>size || element<=0)
            throw new IllegalArgumentException("Invalid transient allocation");
    }
    @Override public ByteBuffer allocateCpu(long size,long alignment,long minimum,long element) {
        validate(size,alignment,minimum,element);
        var a=cpu.allocate(size,alignment,minimum,element);
        return MemoryUtil.memByteBuffer(a.block().address()+a.offset(),Math.toIntExact(a.size())).order(ByteOrder.nativeOrder());
    }
    @Override public GpuBufferSlice allocateGpu(long size,long alignment,int usage,long minimum,long element) {
        validate(size,alignment,minimum,element);
        var allocator=gpu.computeIfAbsent(usage,key->new TransientBlockAllocator<>(524288,Long.highestOneBit(Long.MAX_VALUE),
            Allocator.create(bytes->new Block(renderer.createFenceRotatedBuffer(key,bytes,batch,NativeBuffer.WriteSource.TRANSIENT)),block->block.buffer().close())));
        var a=allocator.allocate(size,allocationAlignment(alignment,hostCoherentAtomSize),minimum,element);
        return a.block().buffer().slice(a.offset(),a.size());
    }
    @Override public GpuBufferSlice.MappedView allocateStaging(long size,long alignment,int usage,long minimum,long element) {
        return mapped(size,alignment,usage|GpuBuffer.USAGE_COPY_SRC,minimum,element);
    }
    @Override public GpuBufferSlice.MappedView allocateGpuMapped(long size,long alignment,int usage,long minimum,long element) {
        return mapped(size,alignment,usage,minimum,element);
    }
    private GpuBufferSlice.MappedView mapped(long size,long alignment,int usage,long minimum,long element) {
        var slice=allocateGpu(size,alignment,usage|GpuBuffer.USAGE_MAP_WRITE,minimum,element);
        return ((NativeBuffer)slice.buffer()).mapTransient(slice.offset(),slice.length());
    }
    private static long allocationAlignment(long alignment,long atomSize) {
        long a=alignment,b=atomSize;
        while(b!=0) {
            long remainder=a%b;
            a=b;
            b=remainder;
        }
        return Math.multiplyExact(alignment/a,atomSize);
    }
    private static long align(long offset,long alignment) {
        if(alignment<=0) throw new IllegalArgumentException("Invalid alignment");
        return Math.addExact(offset,(alignment-offset%alignment)%alignment);
    }
    private GpuBufferSlice upload(List<ByteBuffer> data,long alignment,int usage,long minimum,long element,boolean staging) {
        long size=0;
        for(var bytes:data) size=align(Math.addExact(size,bytes.remaining()),alignment);
        var slice=allocateGpu(size,alignment,usage|GpuBuffer.USAGE_MAP_WRITE|(staging?GpuBuffer.USAGE_COPY_SRC:0),minimum,element);
        ((NativeBuffer)slice.buffer()).writePersistent(slice.offset(),slice.length(),data,alignment);
        return slice;
    }
    @Override public GpuBufferSlice uploadStaging(List<ByteBuffer> data,long alignment,int usage,long minimum,long element) {
        return upload(data,alignment,usage,minimum,element,true);
    }
    @Override public GpuBufferSlice uploadGpu(List<ByteBuffer> data,long alignment,int usage,long minimum,long element) {
        return upload(data,alignment,usage,minimum,element,false);
    }
    @Override public List<GpuBufferSlice> multiUploadStaging(List<ByteBuffer> data,long alignment,int usage) {
        var slices=new ArrayList<GpuBufferSlice>(data.size());
        for(var bytes:data) slices.add(uploadStaging(bytes,alignment,usage));
        return slices;
    }
    @Override public List<GpuBufferSlice> multiUploadGpu(List<ByteBuffer> data,long alignment,int usage) {
        var slices=new ArrayList<GpuBufferSlice>(data.size());
        for(var bytes:data) slices.add(uploadGpu(bytes,alignment,usage));
        return slices;
    }
    void seal(CommandTape tape) {
        cpu.rotate().run();
        for(var allocator:gpu.values()) tape.recycleAfterBatch(allocator.rotate());
    }
    @Override public void close() {
        gpu.values().forEach(TransientBlockAllocator::close);
        cpu.close();
    }
}
