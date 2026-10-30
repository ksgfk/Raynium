package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;

/** The consumer must release all submitted output sequences before close. */
public final class NativeRenderTarget implements AutoCloseable {
    final NativeRenderer owner;
    MemorySegment handle=MemorySegment.NULL;
    SharedImageInfo info;
    NativeRenderTarget(NativeRenderer owner) { this.owner=owner; }
    public SharedImageInfo info() { checkOpen(); return info; }
    void checkOpen() {
        owner.checkOpen();
        if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Render target is closed");
    }
    @Override public void close() {
        owner.checkThread();
        if(handle.equals(MemorySegment.NULL)) return;
        NativeBindings.call(owner.bindings.destroyRenderTarget,"destroy render target",handle);
        handle=MemorySegment.NULL; owner.target=null;
    }
}
