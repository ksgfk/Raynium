package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import java.nio.file.Path;
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
            SymbolLookup lookup = SymbolLookup.libraryLookup(windowsLibraryPath(payload.libraryPath()), arena);
            bindings = new NativeBindings(lookup);
            int abiVersion = bindings.inspect(payload.manifest());
            NativeManifest manifest = payload.manifest();
            logger.info("Raynium native loaded from {}: {}", payload.source(), payload.libraryPath());
            logger.info("Native ABI={} build={} RadRay={} dirty={} config={} CRT={} toolchain={}",
                        abiVersion, manifest.buildId(), manifest.radrayCommit(), manifest.radrayDirty(),
                        manifest.configuration(), manifest.crt(), manifest.toolchain());
        } catch (RuntimeException | Error failure) {
            arena.close();
            throw failure;
        }
    }

    static String windowsLibraryPath(Path path) {
        String absolute = path.toAbsolutePath().normalize().toString();
        // The Path overload resolves toRealPath and strips the prefix before LoadLibrary.
        if (absolute.startsWith("\\\\?\\"))
            return absolute;
        return absolute.startsWith("\\\\")
            ? "\\\\?\\UNC\\" + absolute.substring(2)
            : "\\\\?\\" + absolute;
    }

    NativeBindings bindings() {
        if (closed)
            throw new IllegalStateException("Native library is closed");
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
