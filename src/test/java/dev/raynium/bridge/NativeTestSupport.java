package dev.raynium.bridge;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.reflect.Proxy;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.slf4j.Logger;

/** Owns raw C ABI handles so assertion failures cannot unload a DLL with live runtimes. */
final class NativeTestSupport implements AutoCloseable {
    private final Arena arena = Arena.ofConfined();
    private final List<MemorySegment> runtimes = new ArrayList<>();
    private final MethodHandle create;
    private final MethodHandle query;
    private final MethodHandle initEnv;
    private final MethodHandle destroyEnv;
    private boolean ownsEnvironment;
    private final MethodHandle destroy;
    final MemorySegment envDesc;
    final MemorySegment desc;
    final MemorySegment output;
    final MemorySegment error;
    final MemorySegment info;

    NativeTestSupport(Path gameDirectory) throws IOException {
        try {
            var payload = locate(gameDirectory);
            var symbols = SymbolLookup.libraryLookup(payload.libraryPath(), arena);
            new NativeBindings(symbols).inspect(payload.manifest());
            var linker = Linker.nativeLinker();
            initEnv = linker.downcallHandle(symbols.find("rn_init_env").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            destroyEnv = linker.downcallHandle(symbols.find("rn_destroy_env").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            query = linker.downcallHandle(symbols.find("rn_query_abi").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            create = linker.downcallHandle(symbols.find("rn_create_runtime").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                            ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            destroy = linker.downcallHandle(symbols.find("rn_destroy_runtime").orElseThrow(),
                    FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
            envDesc = arena.allocate(12, 4);
            envDesc.set(ValueLayout.JAVA_INT, 0, 12);
            envDesc.set(ValueLayout.JAVA_INT, 4, 1);
            envDesc.set(ValueLayout.JAVA_INT, 8, Boolean.getBoolean("raynium.rhi.validation") ? 1 : 0);
            desc = arena.allocate(20, 4);
            desc.set(ValueLayout.JAVA_INT, 0, 20);
            output = arena.allocate(ValueLayout.ADDRESS);
            error = arena.allocate(256);
            info = arena.allocate(240, 4);
        } catch (IOException | RuntimeException | Error failure) {
            arena.close();
            throw failure;
        }
    }

    static NativeLibraryLocator.Payload locate(Path gameDirectory) throws IOException {
        String directory = System.getProperty("raynium.native.dir");
        if (directory == null || directory.isBlank()) {
            throw new IllegalStateException("Run the Gradle nativeTest task to prepare and select the native payload");
        }
        if (!NativeTestSupport.class.getModule().isNativeAccessEnabled()) {
            throw new IllegalStateException("The test JVM requires --enable-native-access=ALL-UNNAMED; use nativeTest");
        }
        return new NativeLibraryLocator(gameDirectory, null).locate().orElseThrow();
    }

    static Logger recordingLogger(List<String> events) {
        return (Logger) Proxy.newProxyInstance(Logger.class.getClassLoader(), new Class<?>[]{Logger.class},
                (proxy, method, values) -> {
                    if (method.getReturnType() == boolean.class) return true;
                    if (method.getName().equals("getName")) return "native-test";
                    if (method.getName().equals("info")) events.add((String) values[0]);
                    return null;
                });
    }

    int query(MemorySegment info, int capacity, MemorySegment diagnostic, int diagnosticCapacity) throws Throwable {
        return (int) query.invokeExact(info, capacity, diagnostic, diagnosticCapacity);
    }

    int initEnv(MemorySegment descriptor, MemorySegment diagnostic, int capacity) throws Throwable {
        int status = (int) initEnv.invokeExact(descriptor, diagnostic, capacity);
        if (status == 0) ownsEnvironment = true;
        return status;
    }

    int destroyEnv(MemorySegment diagnostic, int capacity) throws Throwable {
        int status = (int) destroyEnv.invokeExact(diagnostic, capacity);
        if (status == 0) ownsEnvironment = false;
        return status;
    }

    int create(MemorySegment descriptor, MemorySegment out, MemorySegment diagnostic, int capacity) throws Throwable {
        int status = (int) create.invokeExact(descriptor, out, diagnostic, capacity);
        if (status == 0 && !out.equals(MemorySegment.NULL)) {
            MemorySegment handle = out.get(ValueLayout.ADDRESS, 0);
            if (!handle.equals(MemorySegment.NULL)) runtimes.add(handle);
        }
        return status;
    }

    void destroy(MemorySegment handle) {
        try {
            destroy.invokeExact(handle);
            runtimes.remove(handle);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot invoke rn_destroy_runtime", failure);
        }
    }

    @Override
    public void close() {
        while (!runtimes.isEmpty()) destroy(runtimes.getLast());
        if (ownsEnvironment) {
            try {
                int status = destroyEnv(error, 256);
                if (status != 0) throw new IllegalStateException(error.getString(0));
            } catch (RuntimeException | Error failure) {
                throw failure;
            } catch (Throwable failure) {
                throw new IllegalStateException("Cannot destroy test environment", failure);
            }
        }
        arena.close();
    }
}
