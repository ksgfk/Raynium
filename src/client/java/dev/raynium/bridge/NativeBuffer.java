package dev.raynium.bridge;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import dev.raynium.client.renderpearl.CommandTape;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.function.Supplier;
import static java.lang.foreign.ValueLayout.*;

public final class NativeBuffer implements GpuBuffer {
    private final NativeRenderer owner;
    private final Supplier<CommandTape> batch;
    private final int id,usage;
    private final long size,address;
    private final ArrayList<Mapping> views=new ArrayList<>();
    private final Arena bufferArena;
    private final MemorySegment base;
    private Mapping cachedWrite;
    private boolean trackedDirty;
    private long[] ranges;
    private int rangeCount;
    private boolean closed,released;
    private int references;
    private long lastRetainedTapeId;

    static NativeBuffer create(NativeRenderer owner,MemorySegment renderer,int usage,long size,ByteBuffer initial,Supplier<CommandTape> batch) {
        if(size<=0 || (usage&~1023)!=0 || (usage&~USAGE_HINT_CLIENT_STORAGE)==0 || (initial!=null && initial.remaining()!=size))
            throw new IllegalArgumentException("Invalid buffer descriptor");
        if((usage&USAGE_MAP_READ)!=0 && (usage&~(USAGE_MAP_READ|USAGE_COPY_DST|USAGE_HINT_CLIENT_STORAGE))!=0)
            throw new UnsupportedOperationException("MAP_READ buffer cannot have GPU read or write mapping usages");
        if(initial!=null && !initial.isDirect()) {
            try(var scratch=Arena.ofConfined()) {
                var copied=scratch.allocate(size,1);
                copied.copyFrom(MemorySegment.ofBuffer(initial));
                return create(owner,renderer,usage,size,copied,batch);
            }
        }
        return create(owner,renderer,usage,size,initial==null?MemorySegment.NULL:MemorySegment.ofBuffer(initial),batch);
    }
    private static NativeBuffer create(NativeRenderer owner,MemorySegment renderer,int usage,long size,MemorySegment initial,Supplier<CommandTape> batch) {
        var desc=owner.bufferDesc;
        desc.set(JAVA_INT,0,24); desc.set(JAVA_INT,4,usage);
        desc.set(JAVA_LONG,8,size); desc.set(ADDRESS,16,initial);
        var result=owner.bufferResult;
        NativeBindings.call(owner.bindings.createBuffer,"create buffer",renderer,desc,result);
        return new NativeBuffer(owner,batch,result.get(JAVA_INT,0),usage,size,result.get(ADDRESS,8).address());
    }
    private NativeBuffer(NativeRenderer owner,Supplier<CommandTape> batch,int id,int usage,long size,long address) {
        this.owner=owner; this.batch=batch; this.id=id; this.usage=usage; this.size=size; this.address=address;
        bufferArena=address==0?null:Arena.ofConfined();
        base=address==0?null:MemorySegment.ofAddress(address).reinterpret(size,bufferArena,null);
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
    public long lastRetainedTapeId() { return lastRetainedTapeId; }
    public void setLastRetainedTapeId(long tapeId) { lastRetainedTapeId=tapeId; }
    public void retainRecording() { ++references; }
    public void releaseRecording() {
        if(references<=0) throw new IllegalStateException("Unbalanced buffer reference");
        --references;
        retire();
    }
    private void retire() {
        if(closed && references==0 && !released) {
            if(bufferArena!=null) bufferArena.close();
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
        for(var view:views) view.arena.close();
        if(bufferArena!=null) bufferArena.close();
        views.clear();
    }
    @Override public GpuBufferSlice.MappedView map(long offset,long length,boolean read,boolean write) {
        if(read==write || length>Integer.MAX_VALUE) throw new IllegalArgumentException("Invalid buffer mapping");
        checkRange(owner,offset,length,read?USAGE_MAP_READ:USAGE_MAP_WRITE);
        if(read) owner.pollCompleted();
        boolean persistent=write && base!=null;
        Mapping mapping;
        if(persistent && cachedWrite==null) cachedWrite=new Mapping();
        if(persistent && cachedWrite.arena==null) mapping=cachedWrite;
        else mapping=new Mapping();
        var view=mapping.open(offset,length,read);
        views.add(mapping);
        retainRecording();
        if(persistent) {
            addRange(offset,length);
            if(!trackedDirty) {
                trackedDirty=true;
                retainRecording();
                owner.trackWritable(this);
            }
        }
        return view;
    }

    private void addRange(long start,long length) {
        long end=start+length;
        if(ranges==null) ranges=new long[4];
        int first=0;
        while(first<rangeCount && ranges[first*2+1]<start) ++first;
        int after=first;
        while(after<rangeCount && ranges[after*2]<=end) {
            start=Math.min(start,ranges[after*2]);
            end=Math.max(end,ranges[after*2+1]);
            ++after;
        }
        int count=rangeCount-(after-first)+1;
        if(count*2>ranges.length) ranges=Arrays.copyOf(ranges,ranges.length*2);
        System.arraycopy(ranges,2*after,ranges,2*(first+1),2*(rangeCount-after));
        ranges[2*first]=start; ranges[2*first+1]=end;
        rangeCount=count;
    }
    int rangeCount() { return rangeCount; }
    long encodeRanges(MemorySegment dirty,long at) {
        for(int i=0;i<rangeCount;i++,at+=24) {
            dirty.set(JAVA_INT,at,id); dirty.set(JAVA_INT,at+4,0);
            dirty.set(JAVA_LONG,at+8,ranges[2*i]);
            dirty.set(JAVA_LONG,at+16,ranges[2*i+1]-ranges[2*i]);
        }
        return at;
    }
    boolean afterSubmit() {
        rangeCount=0;
        for(int i=0;i<views.size();i++) {
            var view=views.get(i);
            addRange(view.offset,view.memory.byteSize());
        }
        if(rangeCount!=0) return false;
        trackedDirty=false;
        releaseRecording();
        return true;
    }

    private final class Mapping {
        private Arena arena;
        private MemorySegment memory;
        private long offset;
        GpuBufferSlice.MappedView open(long offset,long length,boolean read) {
            this.offset=offset;
            // The returned ByteBuffer must become inaccessible even when other slices remain mapped.
            Arena scope=Arena.ofConfined();
            arena=scope;
            memory=base==null?scope.allocate(length,8):base.asSlice(offset,length).reinterpret(length,scope,null);
            var bytes=memory.asByteBuffer();
            if(read) bytes=bytes.asReadOnlyBuffer();
            return new GpuBufferSlice.MappedView(new GpuBufferSlice(NativeBuffer.this,offset,length),
                bytes.order(ByteOrder.nativeOrder()),()->close(scope));
        }
        private void close(Arena scope) {
            owner.checkThread();
            if(arena!=scope || !scope.scope().isAlive()) return;
            try {
                if(address==0 && !closed) batch.get().writeBuffer(NativeBuffer.this,offset,memory);
            } finally {
                scope.close();
                arena=null; memory=null;
                views.remove(this);
                releaseRecording();
            }
        }
    }
}
