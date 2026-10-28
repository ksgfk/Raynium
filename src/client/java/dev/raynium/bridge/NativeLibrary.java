package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import org.slf4j.Logger;

final class NativeLibrary implements AutoCloseable {
    private final Arena arena;
    private final NativeBindings bindings;
    private boolean closed;

    NativeLibrary(NativeLibraryLocator.Payload payload, Logger logger) {
        if (!NativeLibrary.class.getModule().isNativeAccessEnabled()) {
            throw new IllegalStateException("Enable native access on the Minecraft JVM: --enable-native-access=ALL-UNNAMED");
        }
        arena = Arena.ofShared();
        try {
            SymbolLookup lookup = SymbolLookup.libraryLookup(payload.libraryPath(), arena);
            bindings = new NativeBindings(lookup);
            int abiVersion = bindings.inspect(payload.manifest());
            NativeManifest manifest = payload.manifest();
            logger.info("Raynium native loaded from {}: {}", payload.source(), payload.libraryPath());
            logger.info("Native ABI={} build={} RadRay={} dirty={} config={} CRT=MT toolchain={}",
                    abiVersion, manifest.buildId(), manifest.radrayCommit(), manifest.radrayDirty(),
                    manifest.configuration(), manifest.toolchain());
        } catch (RuntimeException | Error failure) {
            arena.close();
            throw failure;
        }
    }

    NativeBindings bindings() {
        if (closed) throw new IllegalStateException("Native library is closed");
        return bindings;
    }

    @Override
    public void close() {
        if (!closed) {
            // The owning NativeEnvironment must destroy its environment before unloading the DLL.
            arena.close();
            closed = true;
        }
    }
}
