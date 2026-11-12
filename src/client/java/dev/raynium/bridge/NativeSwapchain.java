package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import static java.lang.foreign.ValueLayout.*;

/** HWND is borrowed. Close this output before its renderer and before destroying the window. */
public final class NativeSwapchain implements AutoCloseable {
    public enum PresentMode { IMMEDIATE, FIFO }
    public enum Presentation { PRESENTED, SKIPPED, RECREATE }
    private static final Presentation[] OUTCOMES=Presentation.values();
    final NativeRenderer owner;
    private final long hwnd;
    private final Arena arena=Arena.ofConfined();
    final MemorySegment result=arena.allocate(16,8);
    MemorySegment handle=MemorySegment.NULL;
    NativeSwapchain(NativeRenderer owner,long hwnd) { this.owner=owner; this.hwnd=hwnd; result.set(JAVA_INT,0,16); }
    static MemorySegment descriptor(Arena arena,long hwnd,int width,int height,PresentMode mode) {
        var desc=arena.allocate(24,8); desc.set(JAVA_INT,0,24); desc.set(JAVA_INT,4,width); desc.set(JAVA_INT,8,height);
        desc.set(JAVA_INT,12,java.util.Objects.requireNonNull(mode).ordinal()); desc.set(JAVA_LONG,16,hwnd); return desc;
    }
    public void resize(int width,int height,PresentMode mode) {
        checkOpen();
        try(var scratch=Arena.ofConfined()) {
            NativeBindings.call(owner.bindings.resizeSwapchain,"resize swapchain",handle,descriptor(scratch,hwnd,width,height,mode));
        }
    }
    public long acquireTimeouts() { checkOpen(); return result.get(JAVA_LONG,8); }
    Presentation presentation() {
        int value=result.get(JAVA_INT,4);
        if(result.get(JAVA_INT,0)!=16 || value<0 || value>=OUTCOMES.length) throw new IllegalStateException("Invalid native presentation result");
        return OUTCOMES[value];
    }
    void checkOpen() {
        owner.checkOpen();
        if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Swapchain is closed");
    }
    void closeScratch() { arena.close(); }
    @Override public void close() {
        owner.checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        NativeBindings.call(owner.bindings.destroySwapchain,"destroy swapchain",handle);
        handle=MemorySegment.NULL; owner.swapchain=null; arena.close();
    }
}
