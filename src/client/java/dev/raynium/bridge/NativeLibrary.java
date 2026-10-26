package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import org.slf4j.Logger;

public final class NativeLibrary implements AutoCloseable {
    private final Arena arena;
    private boolean closed;

    public NativeLibrary(NativeLibraryLocator.Payload payload, Logger logger) {
        if (!NativeLibrary.class.getModule().isNativeAccessEnabled()) {
            throw new IllegalStateException("Enable native access on the Minecraft JVM: --enable-native-access=ALL-UNNAMED");
        }
        arena = Arena.ofShared();
        try {
            SymbolLookup lookup = SymbolLookup.libraryLookup(payload.libraryPath(), arena);
            NativeBindings.Probe probe = new NativeBindings(lookup).inspect(payload.manifest());
            NativeManifest manifest = payload.manifest();
            logger.info("Raynium native loaded from {}: {}", payload.source(), payload.libraryPath());
            logger.info("Native ABI={} build={} RadRay={} dirty={} config={} CRT=MT toolchain={}",
                    probe.abiVersion(), manifest.buildId(), manifest.radrayCommit(), manifest.radrayDirty(),
                    manifest.configuration(), manifest.toolchain());
            logger.info("Native probes: RadRay vertex stride={}, mimalloc allocation mask={}", probe.vertexStride(), probe.allocatorMask());
        } catch (RuntimeException | Error failure) {
            arena.close();
            throw failure;
        }
    }

    @Override
    public void close() {
        if (!closed) {
            // Future runtime workers and callbacks must stop before the library arena is closed.
            arena.close();
            closed = true;
        }
    }
}
