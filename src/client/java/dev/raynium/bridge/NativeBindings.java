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
            ValueLayout.JAVA_INT.withName("struct_size"), ValueLayout.JAVA_INT.withName("flags"),
            ValueLayout.JAVA_INT.withName("node_mask"),
            MemoryLayout.sequenceLayout(8, ValueLayout.JAVA_BYTE).withName("luid"));
    private final MethodHandle version;
    private final MethodHandle query;
    private final MethodHandle initEnvironment;
    private final MethodHandle destroyEnvironment;
    private final MethodHandle create;
    private final MethodHandle destroy;
    private final MethodHandle createPreview;
    private final MethodHandle drawPreview;
    private final MethodHandle destroyPreview;

    NativeBindings(SymbolLookup library) {
        Linker linker = Linker.nativeLinker();
        FunctionDescriptor scalar = FunctionDescriptor.of(ValueLayout.JAVA_INT);
        version = bind(linker, library, "rn_get_abi_version", scalar);
        query = bind(linker, library, "rn_query_abi", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        initEnvironment = bind(linker, library, "rn_init_env",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        destroyEnvironment = bind(linker, library, "rn_destroy_env",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        create = bind(linker, library, "rn_create_runtime", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        destroy = bind(linker, library, "rn_destroy_runtime", FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
        createPreview = bind(linker, library, "rn_create_preview", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        drawPreview = bind(linker, library, "rn_draw_preview", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        destroyPreview = bind(linker, library, "rn_destroy_preview", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
    }

    int inspect(NativeManifest expected) {
        try (Arena scratch = Arena.ofConfined()) {
            int abi = (int) version.invokeExact();
            if (abi != NativeManifest.ABI_VERSION || abi != expected.abiVersion()) {
                throw new IllegalStateException("Native ABI version mismatch: " + abi);
            }
            MemorySegment info = scratch.allocate(abiLayout);
            MemorySegment error = scratch.allocate(1024);
            int status = (int) query.invokeExact(info, (int) abiLayout.byteSize(), error, 1024);
            if (status != 0) throw new IllegalStateException("rn_query_abi failed: " + error.getString(0));
            int flags = 6 | (expected.radrayDirty() ? 1 : 0);
            if (number(info, "struct_size") != expected.abiInfoSize()
                    || number(info, "abi_version") != abi
                    || number(info, "pointer_size") != ValueLayout.ADDRESS.byteSize()
                    || number(info, "flags") != flags
                    || !text(info, "build_id").equals(expected.buildId())
                    || !text(info, "radray_commit").equals(expected.radrayCommit())
                    || !text(info, "configuration").equals(expected.configuration())
                    || !text(info, "platform").equals(expected.platform())
                    || !text(info, "toolchain").equals(expected.toolchain())) {
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
            MemorySegment error = scratch.allocate(1024);
            int status = (int) initEnvironment.invokeExact(desc, error, 1024);
            if (status != 0) throw new IllegalStateException("rn_init_env failed: " + error.getString(0));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot initialize native environment", failure);
        }
    }

    void destroyEnvironment() {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment error = scratch.allocate(1024);
            int status = (int) destroyEnvironment.invokeExact(error, 1024);
            if (status != 0) throw new IllegalStateException("rn_destroy_env failed: " + error.getString(0));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot destroy native environment", failure);
        }
    }

    MemorySegment createRuntime(NativeRuntime.HostGpu host) {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment desc = scratch.allocate(runtimeDescLayout);
            desc.set(ValueLayout.JAVA_INT, runtimeDescLayout.byteOffset(MemoryLayout.PathElement.groupElement("struct_size")),
                    (int) runtimeDescLayout.byteSize());
            desc.set(ValueLayout.JAVA_INT, 4, host == null ? 0 : 1);
            if (host != null) {
                desc.set(ValueLayout.JAVA_INT, 8, host.nodeMask());
                MemorySegment.copy(MemorySegment.ofArray(host.luid()), 0, desc, 12, 8);
            }
            MemorySegment output = scratch.allocate(ValueLayout.ADDRESS);
            MemorySegment error = scratch.allocate(1024);
            int status = (int) create.invokeExact(desc, output, error, 1024);
            if (status != 0) {
                throw new IllegalStateException("rn_create_runtime failed, status="
                        + status + ": " + error.getString(0));
            }
            MemorySegment handle = output.get(ValueLayout.ADDRESS, 0);
            if (handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Native runtime returned a null handle on success");
            // The address is owned by the DLL, not by the temporary output slot's arena.
            return handle;
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot create the Raynium runtime", failure);
        }
    }

    void destroyRuntime(MemorySegment handle) {
        try {
            destroy.invokeExact(handle);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot destroy the Raynium runtime", failure);
        }
    }

    record CreatedPreview(MemorySegment handle, NativePreview.Info info) {}

    CreatedPreview createPreview(MemorySegment runtime, int width, int height) {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment desc = scratch.allocate(16, 4);
            desc.set(ValueLayout.JAVA_INT, 0, 16);
            desc.set(ValueLayout.JAVA_INT, 4, width);
            desc.set(ValueLayout.JAVA_INT, 8, height);
            MemorySegment output = scratch.allocate(ValueLayout.ADDRESS);
            MemorySegment info = scratch.allocate(128, 8);
            MemorySegment error = scratch.allocate(1024);
            int status = (int) createPreview.invokeExact(runtime, desc, output, info, 128, error, 1024);
            if (status != 0) throw new IllegalStateException("rn_create_preview: " + error.getString(0));
            MemorySegment handle = output.get(ValueLayout.ADDRESS, 0);
            if (handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Null preview on success");
            try {
                return new CreatedPreview(handle, NativePreview.Info.read(info));
            } catch (RuntimeException | Error failure) {
                // Creation has not submitted GPU work, so an unreadable descriptor is safe to release.
                try { destroyPreview(handle); } catch (RuntimeException cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot create preview", failure);
        }
    }

    void drawPreview(MemorySegment preview, long sequence) {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment error = scratch.allocate(1024);
            int status = (int) drawPreview.invokeExact(preview, sequence, error, 1024);
            if (status != 0) throw new IllegalStateException("rn_draw_preview: " + error.getString(0));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot draw preview", failure);
        }
    }

    void destroyPreview(MemorySegment preview) {
        try (Arena scratch = Arena.ofConfined()) {
            MemorySegment error = scratch.allocate(1024);
            int status = (int) destroyPreview.invokeExact(preview, error, 1024);
            if (status != 0) throw new IllegalStateException("rn_destroy_preview: " + error.getString(0));
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot destroy preview", failure);
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
