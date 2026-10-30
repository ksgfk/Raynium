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
    final MethodHandle createRenderer, destroyRenderer, createRenderTarget, destroyRenderTarget,
            createSharedTextures, destroySharedTextures, renderFrame;

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
        createRenderer = bind(linker,library,"rn_create_renderer", signature(3));
        destroyRenderer = bind(linker,library,"rn_destroy_renderer", signature(1));
        createRenderTarget = bind(linker,library,"rn_create_render_target", FunctionDescriptor.of(ValueLayout.JAVA_INT,
            ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.JAVA_INT,ValueLayout.ADDRESS,ValueLayout.JAVA_INT));
        destroyRenderTarget = bind(linker,library,"rn_destroy_render_target", signature(1));
        createSharedTextures = bind(linker,library,"rn_create_shared_textures", FunctionDescriptor.of(ValueLayout.JAVA_INT,
            ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.JAVA_INT,ValueLayout.ADDRESS,ValueLayout.JAVA_INT));
        destroySharedTextures = bind(linker,library,"rn_destroy_shared_textures", signature(1));
        renderFrame = bind(linker,library,"rn_render_frame", signature(3));
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

    private static FunctionDescriptor signature(int pointers) {
        var layouts=new java.lang.foreign.MemoryLayout[pointers+2];
        java.util.Arrays.fill(layouts,ValueLayout.ADDRESS); layouts[pointers+1]=ValueLayout.JAVA_INT;
        return FunctionDescriptor.of(ValueLayout.JAVA_INT,layouts);
    }
    static void call(MethodHandle method, String operation, Object... arguments) {
        try(var arena=Arena.ofConfined()) {
            var error=arena.allocate(1024);
            var args=new java.util.ArrayList<Object>(java.util.Arrays.asList(arguments));
            args.add(error); args.add(1024);
            int status=(int)method.invokeWithArguments(args);
            if(status!=0) throw new IllegalStateException(operation+": "+error.getString(0));
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException(operation,failure); }
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
