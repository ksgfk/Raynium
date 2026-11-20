package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import static java.lang.foreign.ValueLayout.*;

/** HWND is borrowed. Close this output before its renderer and before destroying the window. */
public final class NativeSwapchain implements AutoCloseable {
    public enum PresentMode { IMMEDIATE, FIFO }
    final NativeRenderer owner;
    private final long hwnd;
    MemorySegment handle=MemorySegment.NULL;
    NativeSwapchain(NativeRenderer owner,long hwnd) { this.owner=owner; this.hwnd=hwnd; }
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
    public void checkRecordingOwner(NativeRenderer renderer) {
        checkOpen();
        if(owner!=renderer) throw new IllegalArgumentException("Swapchain belongs to another renderer");
    }
    void checkOpen() {
        owner.checkOpen();
        if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Swapchain is closed");
    }
    @Override public void close() {
        owner.checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        NativeBindings.call(owner.bindings.destroySwapchain,"destroy swapchain",handle);
        handle=MemorySegment.NULL; owner.swapchain=null;
    }
}
