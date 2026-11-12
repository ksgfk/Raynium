package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import org.slf4j.Logger;

/** Owns a device session; the parent environment must outlive it. */
public final class NativeRuntime implements AutoCloseable {
    private final NativeEnvironment environment;
    private final Logger logger;
    private final NativeBindings bindings;
    private MemorySegment handle = MemorySegment.NULL;
    private boolean closed;
    private NativeRenderer renderer;

    NativeRuntime(NativeEnvironment environment, NativeBindings bindings, Logger logger) {
        this.environment = environment;
        this.bindings = bindings;
        this.logger = logger;
        try {
            handle = bindings.createRuntime();
            logger.info("Raynium runtime created: owner={}", Thread.currentThread().getName());
        } catch (RuntimeException | Error failure) {
            if (!handle.equals(MemorySegment.NULL)) bindings.destroyRuntime(handle);
            throw failure;
        }
    }

    public NativeRenderer createRenderer(NativeShaderPack shaders) {
        return createRenderer(shaders,TerrainSubmission.fromProperty(System.getProperty("raynium.terrain.submission","auto")));
    }
    public NativeDeviceInfo deviceInfo() {
        checkOpen();
        try(var arena=java.lang.foreign.Arena.ofConfined()) {
            var info=arena.allocate(NativeDeviceInfo.BYTE_SIZE,8);
            NativeBindings.call(bindings.queryDeviceInfo,"query device info",handle,info,NativeDeviceInfo.BYTE_SIZE);
            return NativeDeviceInfo.read(info);
        }
    }
    public NativeRenderer createRenderer(NativeShaderPack shaders,TerrainSubmission submission) {
        checkOpen();
        if(renderer!=null) throw new IllegalStateException("Runtime already has a renderer");
        renderer=NativeRenderer.create(this,bindings,handle,java.util.Objects.requireNonNull(shaders),java.util.Objects.requireNonNull(submission));
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
