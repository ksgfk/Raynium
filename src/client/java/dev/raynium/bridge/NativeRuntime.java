package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.util.Objects;
import org.slf4j.Logger;

/** Owns the native runtime and its library; all lifecycle calls belong to the creating thread. */
public final class NativeRuntime implements AutoCloseable {
    public enum Backend {
        D3D12(1, "d3d12"), VULKAN(2, "vulkan");

        private final int nativeValue;
        private final String propertyValue;

        Backend(int nativeValue, String propertyValue) {
            this.nativeValue = nativeValue;
            this.propertyValue = propertyValue;
        }

        int nativeValue() { return nativeValue; }
        public String propertyValue() { return propertyValue; }

        public static Backend fromProperty(String value) {
            return switch (value) {
                case "d3d12" -> D3D12;
                case "vulkan" -> VULKAN;
                default -> throw new IllegalArgumentException("Invalid raynium.rhi.backend='" + value
                        + "'; expected d3d12 or vulkan");
            };
        }
    }

    private final Thread owner;
    private final Logger logger;
    private final NativeLibrary library;
    private MemorySegment handle = MemorySegment.NULL;
    private boolean closed;

    public NativeRuntime(NativeLibraryLocator.Payload payload, Backend backend, Logger logger) {
        Objects.requireNonNull(backend, "backend");
        this.logger = Objects.requireNonNull(logger, "logger");
        owner = Thread.currentThread();
        library = new NativeLibrary(payload, logger);
        try {
            handle = library.bindings().createRuntime(backend);
            logger.info("Raynium runtime created: backend={} owner={}; vanilla rendering remains active",
                    backend.propertyValue(), owner.getName());
        } catch (RuntimeException | Error failure) {
            close();
            throw failure;
        }
    }

    @Override
    public void close() {
        if (Thread.currentThread() != owner) {
            throw new IllegalStateException("Raynium runtime must be closed on its creating thread: " + owner.getName());
        }
        if (closed) return;
        if (!handle.equals(MemorySegment.NULL)) {
            library.bindings().destroyRuntime(handle);
            handle = MemorySegment.NULL;
            logger.info("Raynium runtime destroyed");
        }
        library.close();
        closed = true;
        logger.info("Raynium native library closed");
    }
}
