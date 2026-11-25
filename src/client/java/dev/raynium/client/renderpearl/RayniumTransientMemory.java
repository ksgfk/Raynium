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
import java.util.HashSet;
import java.util.List;
import java.util.function.Supplier;
import org.lwjgl.system.MemoryUtil;

final class RayniumTransientMemory implements TransientMemory,AutoCloseable {
    private record Block(NativeBuffer buffer) implements Allocator.Block {
        @Override public boolean suboptimal() { return false; }
    }
    private final TransientBlockAllocator<Allocator.CpuBlock> cpu=new TransientBlockAllocator<>(524288,16,Allocator.CpuBlock.memalloc());
    private final HashMap<Integer,TransientBlockAllocator<Block>> gpu=new HashMap<>();
    private final HashSet<Runnable> pending=new HashSet<>();
    private final ArrayList<GpuBufferSlice.MappedView> scopedViews=new ArrayList<>();
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
        var view=allocateGpu(size,alignment,usage|GpuBuffer.USAGE_MAP_WRITE,minimum,element).map(false,true);
        scopedViews.add(view);
        return view;
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
        try(var view=staging?allocateStaging(size,alignment,usage,minimum,element):allocateGpuMapped(size,alignment,usage,minimum,element)) {
            long offset=0;
            for(var bytes:data) {
                if(offset>=view.slice().length()) break;
                int count=(int)Math.min(bytes.remaining(),view.slice().length()-offset);
                view.data().slice(Math.toIntExact(offset),count).put(bytes.slice(bytes.position(),count));
                offset=align(offset+count,alignment);
            }
            return view.slice();
        }
    }
    @Override public GpuBufferSlice uploadStaging(List<ByteBuffer> data,long alignment,int usage,long minimum,long element) {
        return upload(data,alignment,usage,minimum,element,true);
    }
    @Override public GpuBufferSlice uploadGpu(List<ByteBuffer> data,long alignment,int usage,long minimum,long element) {
        return upload(data,alignment,usage,minimum,element,false);
    }
    @Override public List<GpuBufferSlice> multiUploadStaging(List<ByteBuffer> data,long alignment,int usage) {
        return data.stream().map(bytes->uploadStaging(bytes,alignment,usage)).toList();
    }
    @Override public List<GpuBufferSlice> multiUploadGpu(List<ByteBuffer> data,long alignment,int usage) {
        return data.stream().map(bytes->uploadGpu(bytes,alignment,usage)).toList();
    }
    void seal(CommandTape tape) {
        scopedViews.forEach(GpuBufferSlice.MappedView::close);
        scopedViews.clear();
        cpu.rotate().run();
        for(var allocator:gpu.values()) {
            var recycle=allocator.rotate();
            pending.add(recycle);
            tape.onCompleted(()->{ if(pending.remove(recycle)) recycle.run(); });
        }
    }
    @Override public void close() {
        scopedViews.forEach(GpuBufferSlice.MappedView::close);
        scopedViews.clear();
        pending.forEach(Runnable::run);
        pending.clear();
        gpu.values().forEach(TransientBlockAllocator::close);
        cpu.close();
    }
}
