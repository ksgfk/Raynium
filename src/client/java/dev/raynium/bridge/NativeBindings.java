package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.util.Arrays;
final class NativeBindings {
    private final StructLayout abiLayout = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("struct_size"), ValueLayout.JAVA_INT.withName("abi_version"),
        ValueLayout.JAVA_INT.withName("pointer_size"), ValueLayout.JAVA_INT.withName("flags"),
        MemoryLayout.sequenceLayout(72, ValueLayout.JAVA_BYTE).withName("build_id"),
        MemoryLayout.sequenceLayout(48, ValueLayout.JAVA_BYTE).withName("radray_commit"),
        MemoryLayout.sequenceLayout(16, ValueLayout.JAVA_BYTE).withName("configuration"),
        MemoryLayout.sequenceLayout(24, ValueLayout.JAVA_BYTE).withName("platform"),
        MemoryLayout.sequenceLayout(64, ValueLayout.JAVA_BYTE).withName("toolchain"));
    private final StructLayout runtimeDescLayout = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("struct_size"), ValueLayout.JAVA_INT.withName("reserved"));
    private final MethodHandle version;
    private final MethodHandle query;
    private final MethodHandle initEnvironment;
    private final MethodHandle destroyEnvironment;
    private final MethodHandle create;
    private final MethodHandle destroy;
    static final int BATCH_BYTES = 128, WAIT_BYTES = 48, SUBMIT_BYTES = 56, TELEMETRY_BYTES = 152;
    final MethodHandle createRenderer, destroyRenderer, flushRenderer, drainTelemetry;
    final MethodHandle queryDeviceInfo, createSwapchain, resizeSwapchain, destroySwapchain;
    final MethodHandle createBuffer, destroyBuffer, registerPipeline, destroyPipeline;
    final MethodHandle createQueryPool, destroyQueryPool, syncQueryPool;
    final MethodHandle createTexture, uploadTexture, destroyTexture, upsertModelMesh, removeModelMesh, pollCapture, waitSubmit, submitFrame;

    NativeBindings(SymbolLookup library) {
        Linker linker = Linker.nativeLinker();
        FunctionDescriptor scalar = FunctionDescriptor.of(ValueLayout.JAVA_INT);
        version = bind(linker, library, "rn_get_abi_version", scalar);
        requireCurrentVersion();
        query = bind(linker, library, "rn_query_abi", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        initEnvironment = bind(linker, library, "rn_init_env",
                               FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        destroyEnvironment = bind(linker, library, "rn_destroy_env",
                                  scalar);
        create = bind(linker, library, "rn_create_runtime", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        destroy = bind(linker, library, "rn_destroy_runtime", signature(1));
        createRenderer = bind(linker, library, "rn_create_renderer", signature(3));
        destroyRenderer = bind(linker, library, "rn_destroy_renderer", signature(1));
        queryDeviceInfo = bind(linker, library, "rn_query_device_info", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        createSwapchain = bind(linker, library, "rn_create_swapchain", signature(3));
        resizeSwapchain = bind(linker, library, "rn_resize_swapchain", signature(2));
        destroySwapchain = bind(linker, library, "rn_destroy_swapchain", signature(1));
        createTexture = bind(linker, library, "rn_create_texture", signature(3));
        uploadTexture = bind(linker, library, "rn_upload_texture", signature(2));
        destroyTexture = bind(linker, library, "rn_destroy_texture", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        upsertModelMesh = bind(linker, library, "rn_upsert_model_mesh", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        removeModelMesh = bind(linker, library, "rn_remove_model_mesh", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        pollCapture = bind(linker, library, "rn_poll_capture", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        registerPipeline = bind(linker, library, "rn_register_pipeline", signature(3));
        destroyPipeline = bind(linker, library, "rn_destroy_pipeline", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        createBuffer = bind(linker, library, "rn_create_buffer", signature(3));
        destroyBuffer = bind(linker, library, "rn_destroy_buffer", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        createQueryPool = bind(linker, library, "rn_create_query_pool", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        destroyQueryPool = bind(linker, library, "rn_destroy_query_pool", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        syncQueryPool = bind(linker, library, "rn_sync_query_pool", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        flushRenderer = bind(linker, library, "rn_flush_renderer", signature(2));
        drainTelemetry = bind(linker, library, "rn_drain_telemetry", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        submitFrame = bind(linker, library, "rn_submit_frame", signature(3));
        waitSubmit = bind(linker, library, "rn_wait_submit", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
    }

    private int requireCurrentVersion() {
        try {
            int abi = (int)version.invokeExact();
            if (abi != NativeManifest.ABI_VERSION)
                throw new IllegalStateException("Native ABI version mismatch: " + abi + "; required " + NativeManifest.ABI_VERSION);
            return abi;
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot query the Raynium ABI version", failure);
        }
    }

    int inspect(NativeManifest expected) {
        try (Arena scratch = Arena.ofConfined()) {
            int abi = requireCurrentVersion();
            if (abi != expected.abiVersion()) {
                throw new IllegalStateException("Native ABI version mismatch: " + abi);
            }
            MemorySegment info = scratch.allocate(abiLayout);
            int status = (int)query.invokeExact(info, (int)abiLayout.byteSize());
            checkStatus("rn_query_abi", status);
            int flags = 6 | (expected.radrayDirty() ? 1 : 0);
            if (number(info, "struct_size") != expected.abiInfoSize() || number(info, "abi_version") != abi || number(info, "pointer_size") != ValueLayout.ADDRESS.byteSize() || number(info, "flags") != flags || !text(info, "build_id").equals(expected.buildId()) || !text(info, "radray_commit").equals(expected.radrayCommit()) || !text(info, "configuration").equals(expected.configuration()) || !text(info, "platform").equals(expected.platform()) || !text(info, "toolchain").equals(expected.toolchain())) {
                throw new IllegalStateException("Loaded DLL build identity/layout does not match native-manifest.json");
            }
            return abi;
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            // MethodHandle.invokeExact declares Throwable even for these non-throwing C functions.
            throw new IllegalStateException("Cannot invoke the Raynium C ABI", failure);
        }
    }

    void initEnvironment(NativeEnvironment.Backend backend, boolean validation) {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment desc = scratch.allocate(12, 4);
            desc.set(ValueLayout.JAVA_INT, 0, 12);
            desc.set(ValueLayout.JAVA_INT, 4, backend.nativeValue());
            desc.set(ValueLayout.JAVA_INT, 8, validation ? 1 : 0);
            int status = (int)initEnvironment.invokeExact(desc);
            checkStatus("rn_init_env", status);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot initialize native environment", failure);
        }
    }

    void destroyEnvironment() {
        try {
            int status = (int)destroyEnvironment.invokeExact();
            checkStatus("rn_destroy_env", status);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot destroy native environment", failure);
        }
    }

    MemorySegment createRuntime() {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment desc = scratch.allocate(runtimeDescLayout);
            desc.set(ValueLayout.JAVA_INT, runtimeDescLayout.byteOffset(MemoryLayout.PathElement.groupElement("struct_size")),
                     (int)runtimeDescLayout.byteSize());
            MemorySegment output = scratch.allocate(ValueLayout.ADDRESS);
            int status = (int)create.invokeExact(desc, output);
            checkStatus("rn_create_runtime", status);
            MemorySegment handle = output.get(ValueLayout.ADDRESS, 0);
            if (handle.equals(MemorySegment.NULL))
                throw new IllegalStateException("Native runtime returned a null handle on success");
            // The address is owned by the DLL, not by the temporary output slot's arena.
            return handle;
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot create the Raynium runtime", failure);
        }
    }

    void destroyRuntime(MemorySegment handle) {
        call(destroy, "rn_destroy_runtime", handle);
    }

    private static FunctionDescriptor signature(int pointers) {
        var layouts = new MemoryLayout[pointers];
        Arrays.fill(layouts, ValueLayout.ADDRESS);
        return FunctionDescriptor.of(ValueLayout.JAVA_INT, layouts);
    }
    private static void checkStatus(String operation, int status) {
        if (status != 0)
            throw new IllegalStateException(operation + " failed, status=" + status);
    }
    static void call(MethodHandle method, String operation, MemorySegment handle) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, MemorySegment desc) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, desc));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, MemorySegment desc, MemorySegment out) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, desc, out));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, int id) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, id));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, long pointer) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, pointer));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, int count, MemorySegment out) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, count, out));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, MemorySegment info, int size) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, info, size));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, MemorySegment entries, int capacity, MemorySegment count) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, entries, capacity, count));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, int id, MemorySegment vertices, int bytes, int count, int indices, MemorySegment page) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, id, vertices, bytes, count, indices, page));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    static void call(MethodHandle method, String operation, MemorySegment handle, long sequence, MemorySegment info, MemorySegment pixels, long size) {
        try {
            checkStatus(operation, (int)method.invokeExact(handle, sequence, info, pixels, size));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(operation, failure);
        }
    }
    private MethodHandle bind(Linker linker, SymbolLookup library, String name, FunctionDescriptor descriptor) {
        MemorySegment address = library.find(name).orElseThrow(() -> new IllegalStateException("Native symbol is missing: " + name));
        return linker.downcallHandle(address, descriptor);
    }

    private int number(MemorySegment info, String name) {
        return info.get(ValueLayout.JAVA_INT, abiLayout.byteOffset(MemoryLayout.PathElement.groupElement(name)));
    }

    private String text(MemorySegment info, String name) {
        var field = MemoryLayout.PathElement.groupElement(name);
        return info.asSlice(abiLayout.byteOffset(field), abiLayout.select(field).byteSize()).getString(0);
    }
}
