package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import static java.lang.foreign.ValueLayout.*;

/** Device-local geometry. Updates copy caller bytes synchronously and are submitted with the next frame. */
public final class NativeSceneBuffer implements AutoCloseable {
    private final NativeRenderer owner;
    private final int id;
    private final long size;
    private boolean closed;
    NativeSceneBuffer(NativeRenderer owner,int id,long size) { this.owner=owner; this.id=id; this.size=size; }
    public int reference() { checkOpen(); return id|Integer.MIN_VALUE; }
    public long size() { return size; }
    public void upload(long offset,MemorySegment source) {
        checkOpen();
        if(offset<0 || offset>size || source.byteSize()>size-offset) throw new IndexOutOfBoundsException("Scene upload range");
        if(source.byteSize()==0) return;
        try(var arena=Arena.ofConfined()) {
            if(!source.isNative()) { var copy=arena.allocate(source.byteSize(),1); copy.copyFrom(source); source=copy; }
            var desc=arena.allocate(32,8); desc.set(JAVA_INT,0,32); desc.set(JAVA_INT,4,id);
            desc.set(JAVA_LONG,8,offset); desc.set(JAVA_LONG,16,source.byteSize()); desc.set(ADDRESS,24,source);
            owner.uploadSceneBuffer(desc);
        }
    }
    private void checkOpen() { owner.checkOpen(); if(closed) throw new IllegalStateException("Scene buffer is closed"); }
    @Override public void close() {
        owner.checkThread(); if(closed) return;
        if(!owner.isClosed()) owner.destroySceneBuffer(id);
        closed=true;
    }
}
