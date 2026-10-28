package dev.raynium.bridge;

import java.util.Objects;
import org.slf4j.Logger;

/** Owns the DLL and its singleton environment until all device sessions are closed. */
public final class NativeEnvironment implements AutoCloseable {
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


    private final Thread owner = Thread.currentThread();
    private final NativeLibrary library;
    private final Logger logger;
    private NativeRuntime runtime;
    private boolean closed;

    public NativeEnvironment(NativeLibraryLocator.Payload payload, Backend backend, Logger logger) {
        this(payload, backend, Boolean.getBoolean("raynium.rhi.validation"), logger);
    }

    public NativeEnvironment(NativeLibraryLocator.Payload payload, Backend backend, boolean validation, Logger logger) {
        Objects.requireNonNull(backend, "backend");
        this.logger = Objects.requireNonNull(logger, "logger");
        library = new NativeLibrary(payload, logger);
        try {
            library.bindings().initEnvironment(backend, validation);
        } catch (RuntimeException | Error failure) {
            // A failed init never acquires the singleton, including duplicate init.
            library.close();
            throw failure;
        }
        logger.info("Raynium native environment initialized: backend={} driver validation={}", backend.propertyValue(), validation);
    }

    public NativeRuntime createRuntime(NativeRuntime.HostGpu host) {
        checkOpen();
        if (runtime != null) throw new IllegalStateException("Native environment already has a runtime");
        runtime = new NativeRuntime(this, library.bindings(), logger, host);
        return runtime;
    }

    void releasedRuntime(NativeRuntime value) {
        if (runtime != value) throw new IllegalStateException("Runtime owner mismatch");
        runtime = null;
    }

    void checkThread() {
        if (Thread.currentThread() != owner)
            throw new IllegalStateException("Native environment must be used on its initializing thread");
    }

    void checkOpen() {
        checkThread();
        if (closed) throw new IllegalStateException("Native environment is closed");
    }

    @Override public void close() {
        checkThread();
        if (closed) return;
        if (runtime != null) throw new IllegalStateException("Close the runtime before closing the native environment");
        library.bindings().destroyEnvironment();
        logger.info("Raynium native environment destroyed");
        library.close();
        closed = true;
        logger.info("Raynium native library closed");
    }
}
