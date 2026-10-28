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
    private NativePreview preview;

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

    public NativePreview createPreview(int width, int height) {
        checkOpen();
        if (preview != null) throw new IllegalStateException("Runtime already has a preview");
        var created = bindings.createPreview(handle, width, height);
        preview = new NativePreview(this, bindings, created.handle(), created.info());
        return preview;
    }

    void checkOpen() {
        environment.checkOpen();
        if (closed) throw new IllegalStateException("Runtime is closed");
    }

    void releasedPreview(NativePreview value) {
        if (preview != value) throw new IllegalStateException("Preview owner mismatch");
        preview = null;
    }

    void checkThread() {
        environment.checkThread();
    }

    @Override
    public void close() {
        checkThread();
        if (closed) return;
        if (preview != null) throw new IllegalStateException("Close the preview after consumer completion before closing runtime");
        if (!handle.equals(MemorySegment.NULL)) {
            bindings.destroyRuntime(handle);
            handle = MemorySegment.NULL;
            logger.info("Raynium runtime destroyed");
        }
        environment.releasedRuntime(this);
        closed = true;
    }
}
