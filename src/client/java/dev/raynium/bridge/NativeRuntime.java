package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import org.slf4j.Logger;

/** Owns a device session; the parent environment must outlive it. */
public final class NativeRuntime implements AutoCloseable {
    public record HostGpu(byte[] luid, int nodeMask) {
        public HostGpu {
            if (luid.length != 8 || nodeMask != 1) throw new IllegalArgumentException("Expected 8-byte LUID and node mask 1");
            luid = luid.clone();
        }
        @Override public byte[] luid() { return luid.clone(); }
    }
    private final NativeEnvironment environment;
    private final Logger logger;
    private final NativeBindings bindings;
    private MemorySegment handle = MemorySegment.NULL;
    private boolean closed;
    private NativeRenderer renderer;

    NativeRuntime(NativeEnvironment environment, NativeBindings bindings, Logger logger, HostGpu host) {
        this.environment = environment;
        this.bindings = bindings;
        this.logger = logger;
        try {
            handle = bindings.createRuntime(host);
            logger.info("Raynium runtime created: owner={}; vanilla rendering remains active", Thread.currentThread().getName());
        } catch (RuntimeException | Error failure) {
            if (!handle.equals(MemorySegment.NULL)) bindings.destroyRuntime(handle);
            throw failure;
        }
    }

    public NativeRenderer createRenderer() {
        checkOpen();
        if(renderer!=null) throw new IllegalStateException("Runtime already has a renderer");
        renderer=NativeRenderer.create(this,bindings,handle);
        return renderer;
    }
    void releasedRenderer(NativeRenderer value) {
        if(renderer!=value) throw new IllegalStateException("Renderer owner mismatch");
        renderer=null;
    }
    void checkOpen() {
        environment.checkOpen();
        if (closed) throw new IllegalStateException("Runtime is closed");
    }

    void checkThread() {
        environment.checkThread();
    }

    @Override
    public void close() {
        checkThread();
        if (closed) return;
        if (renderer != null) throw new IllegalStateException("Close the renderer before closing runtime");
        if (!handle.equals(MemorySegment.NULL)) {
            bindings.destroyRuntime(handle);
            handle = MemorySegment.NULL;
            logger.info("Raynium runtime destroyed");
        }
        environment.releasedRuntime(this);
        closed = true;
    }
}
