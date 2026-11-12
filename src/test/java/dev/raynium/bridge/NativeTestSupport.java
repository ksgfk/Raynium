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
    static NativeShaderPack shaders() {
        try(var input=NativeTestSupport.class.getResourceAsStream("/assets/raynium/shaderpacks/default.rnsp")) {
            if(input==null) throw new IllegalStateException("Missing test shader pack");
            return NativeShaderPack.read(input);
        } catch(IOException failure) { throw new java.io.UncheckedIOException(failure); }
    }
    static final int[] SOLID_COLOR={64,128,191,0};
    static NativeRenderer.Frame solidFrame(long sequence) {
        var frame=new NativeRenderer.Frame(); frame.outputSequence=sequence;
        frame.fogColor=new float[]{.25f,.5f,.75f,1};
        frame.flags=Boolean.getBoolean("raynium.terrain.profile")?NativeRenderer.PROFILE:0;
        return frame;
    }
    private final Arena arena = Arena.ofConfined();
    private final List<MemorySegment> runtimes = new ArrayList<>();
    private final MethodHandle create;
    private final MethodHandle query;
    private final MethodHandle initEnv;
    private final MethodHandle destroyEnv;
    private boolean ownsEnvironment;
    private final MethodHandle destroy;
    private final MethodHandle createRenderer;
    final MemorySegment envDesc;
    final MemorySegment desc;
    final MemorySegment output;
    final MemorySegment info;

    NativeTestSupport(Path gameDirectory) throws IOException {
        try {
            var payload = locate(gameDirectory);
            var symbols = SymbolLookup.libraryLookup(payload.libraryPath(), arena);
            new NativeBindings(symbols).inspect(payload.manifest());
            var linker = Linker.nativeLinker();
            initEnv = linker.downcallHandle(symbols.find("rn_init_env").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
            destroyEnv = linker.downcallHandle(symbols.find("rn_destroy_env").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT));
            query = linker.downcallHandle(symbols.find("rn_query_abi").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
            create = linker.downcallHandle(symbols.find("rn_create_runtime").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
            destroy = linker.downcallHandle(symbols.find("rn_destroy_runtime").orElseThrow(),
                    FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
            createRenderer = linker.downcallHandle(symbols.find("rn_create_renderer").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
            envDesc = arena.allocate(12, 4);
            envDesc.set(ValueLayout.JAVA_INT, 0, 12);
            envDesc.set(ValueLayout.JAVA_INT, 4, 1);
            envDesc.set(ValueLayout.JAVA_INT, 8, Boolean.getBoolean("raynium.rhi.validation") ? 1 : 0);
            desc = arena.allocate(8, 4);
            desc.set(ValueLayout.JAVA_INT, 0, 8);
            output = arena.allocate(ValueLayout.ADDRESS);
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

    int query(MemorySegment info, int capacity) throws Throwable {
        return (int) query.invokeExact(info, capacity);
    }

    int createRenderer(MemorySegment runtime, MemorySegment descriptor, MemorySegment out) throws Throwable {
        return (int) createRenderer.invokeExact(runtime, descriptor, out);
    }

    int initEnv(MemorySegment descriptor) throws Throwable {
        int status = (int) initEnv.invokeExact(descriptor);
        if (status == 0) ownsEnvironment = true;
        return status;
    }

    int destroyEnv() throws Throwable {
        int status = (int) destroyEnv.invokeExact();
        if (status == 0) ownsEnvironment = false;
        return status;
    }

    int create(MemorySegment descriptor, MemorySegment out) throws Throwable {
        int status = (int) create.invokeExact(descriptor, out);
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
                int status = destroyEnv();
                if (status != 0) throw new IllegalStateException("rn_destroy_env failed, status=" + status);
            } catch (RuntimeException | Error failure) {
                throw failure;
            } catch (Throwable failure) {
                throw new IllegalStateException("Cannot destroy test environment", failure);
            }
        }
        arena.close();
    }
}
