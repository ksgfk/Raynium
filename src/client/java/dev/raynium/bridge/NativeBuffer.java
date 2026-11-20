package dev.raynium.bridge;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import dev.raynium.client.renderpearl.CommandTape;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashSet;
import java.util.function.Supplier;
import static java.lang.foreign.ValueLayout.*;

public final class NativeBuffer implements GpuBuffer {
    private final NativeRenderer owner;
    private final Supplier<CommandTape> batch;
    private final int id,usage;
    private final long size,address;
    private final HashSet<Mapping> views=new HashSet<>();
    private boolean closed,released;
    private int references;

    static NativeBuffer create(NativeRenderer owner,MemorySegment renderer,int usage,long size,ByteBuffer initial,Supplier<CommandTape> batch) {
        if(size<=0 || (usage&~1023)!=0 || (usage&~USAGE_HINT_CLIENT_STORAGE)==0 || (initial!=null && initial.remaining()!=size))
            throw new IllegalArgumentException("Invalid buffer descriptor");
        if((usage&USAGE_MAP_READ)!=0 && (usage&~(USAGE_MAP_READ|USAGE_COPY_DST|USAGE_HINT_CLIENT_STORAGE))!=0)
            throw new UnsupportedOperationException("MAP_READ buffer cannot have GPU read or write mapping usages");
        try(var arena=Arena.ofConfined()) {
            var desc=arena.allocate(24,8);
            desc.set(JAVA_INT,0,24);
            desc.set(JAVA_INT,4,usage);
            desc.set(JAVA_LONG,8,size);
            if(initial!=null) {
                var bytes=MemorySegment.ofBuffer(initial);
                if(!bytes.isNative()) {
                    var copied=arena.allocate(size,1);
                    copied.copyFrom(bytes);
                    bytes=copied;
                }
                desc.set(ADDRESS,16,bytes);
            }
            var result=arena.allocate(16,8);
            NativeBindings.call(owner.bindings.createBuffer,"create buffer",renderer,desc,result);
            return new NativeBuffer(owner,batch,result.get(JAVA_INT,0),usage,size,result.get(ADDRESS,8).address());
        }
    }
    private NativeBuffer(NativeRenderer owner,Supplier<CommandTape> batch,int id,int usage,long size,long address) {
        this.owner=owner; this.batch=batch; this.id=id; this.usage=usage; this.size=size; this.address=address;
    }
    @Override public boolean isClosed() { return closed; }
    @Override public int usage() { return usage; }
    @Override public long size() { return size; }
    public int id() { return id; }
    public void checkRange(NativeRenderer renderer,long offset,long length,int requiredUsage) {
        owner.checkOpen();
        if(closed || owner!=renderer) throw new IllegalStateException("Buffer is closed or belongs to another device");
        if((usage&requiredUsage)!=requiredUsage) throw new IllegalArgumentException("Missing buffer usage");
        if(offset<0 || offset>size || length<=0 || length>size-offset) throw new IllegalArgumentException("Invalid buffer range");
    }
    public void retainRecording() { ++references; }
    public void releaseRecording() {
        if(references<=0) throw new IllegalStateException("Unbalanced buffer reference");
        --references;
        retire();
    }
    private void retire() {
        if(closed && references==0 && !released) {
            owner.destroyBuffer(id);
            owner.bufferReleased(this);
            released=true;
        }
    }
    @Override public void close() {
        owner.checkThread();
        closed=true;
        retire();
    }
    void endRenderer() {
        closed=released=true;
        for(var view:views) view.invalidate();
        views.clear();
    }
    @Override public GpuBufferSlice.MappedView map(long offset,long length,boolean read,boolean write) {
        if(read==write || length>Integer.MAX_VALUE) throw new IllegalArgumentException("Invalid buffer mapping");
        checkRange(owner,offset,length,read?USAGE_MAP_READ:USAGE_MAP_WRITE);
        if(read) owner.pollCompleted();
        var view=new Mapping(offset,length,read);
        views.add(view);
        retainRecording();
        if(view.persistentWrite()) owner.trackWritable(view);
        return view.view;
    }
    final class Mapping {
        private final Arena arena=Arena.ofConfined();
        private final long offset,length;
        private final boolean read;
        private final MemorySegment memory;
        private final GpuBufferSlice.MappedView view;
        private boolean finished;
        Mapping(long offset,long length,boolean read) {
            this.offset=offset; this.length=length; this.read=read;
            memory=address==0?arena.allocate(length,8):MemorySegment.ofAddress(address+offset).reinterpret(length,arena,null);
            var bytes=memory.asByteBuffer();
            if(read) bytes=bytes.asReadOnlyBuffer();
            view=new GpuBufferSlice.MappedView(new GpuBufferSlice(NativeBuffer.this,offset,length),bytes.order(ByteOrder.nativeOrder()),this::close);
        }
        boolean persistentWrite() { return !read && address!=0; }
        void encode(MemorySegment ranges,long at) {
            ranges.set(JAVA_INT,at,id);
            ranges.set(JAVA_LONG,at+8,offset);
            ranges.set(JAVA_LONG,at+16,length);
        }
        private void close() {
            owner.checkThread();
            if(finished) return;
            try {
                if(!read && address==0 && !closed) batch.get().writeBuffer(NativeBuffer.this,offset,memory);
            } finally {
                invalidate();
                views.remove(this);
                if(!persistentWrite()) releaseRecording();
            }
        }
        void invalidate() {
            if(!finished) { finished=true; arena.close(); }
        }
        boolean releaseIfClosed() {
            if(finished) releaseRecording();
            return finished;
        }
    }
}
