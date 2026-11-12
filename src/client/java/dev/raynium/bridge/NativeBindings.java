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
            ValueLayout.JAVA_INT.withName("struct_size"), ValueLayout.JAVA_INT.withName("reserved"));
    private final MethodHandle version;
    private final MethodHandle query;
    private final MethodHandle initEnvironment;
    private final MethodHandle destroyEnvironment;
    private final MethodHandle create;
    private final MethodHandle destroy;
    final MethodHandle createRenderer, destroyRenderer;
    final MethodHandle queryDeviceInfo,createSwapchain,resizeSwapchain,destroySwapchain,presentFrame;
    final MethodHandle createTexture,uploadTexture,destroyTexture;

    NativeBindings(SymbolLookup library) {
        Linker linker = Linker.nativeLinker();
        FunctionDescriptor scalar = FunctionDescriptor.of(ValueLayout.JAVA_INT);
        version = bind(linker, library, "rn_get_abi_version", scalar);
        query = bind(linker, library, "rn_query_abi", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        initEnvironment = bind(linker, library, "rn_init_env",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        destroyEnvironment = bind(linker, library, "rn_destroy_env",
                scalar);
        create = bind(linker, library, "rn_create_runtime", FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        destroy = bind(linker, library, "rn_destroy_runtime", FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
        createRenderer = bind(linker,library,"rn_create_renderer", signature(3));
        destroyRenderer = bind(linker,library,"rn_destroy_renderer", signature(1));
        queryDeviceInfo=bind(linker,library,"rn_query_device_info",FunctionDescriptor.of(ValueLayout.JAVA_INT,
            ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.JAVA_INT));
        createSwapchain=bind(linker,library,"rn_create_swapchain",signature(3));
        resizeSwapchain=bind(linker,library,"rn_resize_swapchain",signature(2));
        destroySwapchain=bind(linker,library,"rn_destroy_swapchain",signature(1));
        presentFrame=bind(linker,library,"rn_present_frame",signature(4));
        createTexture=bind(linker,library,"rn_create_texture",signature(2));
        uploadTexture=bind(linker,library,"rn_upload_texture",signature(2));
        destroyTexture=bind(linker,library,"rn_destroy_texture",FunctionDescriptor.of(ValueLayout.JAVA_INT,
            ValueLayout.ADDRESS,ValueLayout.JAVA_INT));
    }

    int inspect(NativeManifest expected) {
        try (Arena scratch = Arena.ofConfined()) {
            int abi = (int) version.invokeExact();
            if (abi != NativeManifest.ABI_VERSION || abi != expected.abiVersion()) {
                throw new IllegalStateException("Native ABI version mismatch: " + abi);
            }
            MemorySegment info = scratch.allocate(abiLayout);
            int status = (int) query.invokeExact(info, (int) abiLayout.byteSize());
            checkStatus("rn_query_abi",status);
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
            int status = (int) initEnvironment.invokeExact(desc);
            checkStatus("rn_init_env",status);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Cannot initialize native environment", failure);
        }
    }

    void destroyEnvironment() {
        try {
            int status = (int) destroyEnvironment.invokeExact();
            checkStatus("rn_destroy_env",status);
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
                    (int) runtimeDescLayout.byteSize());
            MemorySegment output = scratch.allocate(ValueLayout.ADDRESS);
            int status = (int) create.invokeExact(desc, output);
            checkStatus("rn_create_runtime",status);
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
        var layouts=new MemoryLayout[pointers];
        java.util.Arrays.fill(layouts,ValueLayout.ADDRESS);
        return FunctionDescriptor.of(ValueLayout.JAVA_INT,layouts);
    }
    private static void checkStatus(String operation,int status) {
        if(status!=0) throw new IllegalStateException(operation+" failed, status="+status);
    }
    static void call(MethodHandle method,String operation,Object... arguments) {
        try {
            int status=(int)method.invokeWithArguments(arguments);
            checkStatus(operation,status);
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException(operation,failure); }
    }
    static void callPresentFrame(MethodHandle method,MemorySegment renderer,MemorySegment target,
                                 MemorySegment frame,MemorySegment result) {
        try {
            int status=(int)method.invokeExact(renderer,target,frame,result);
            checkStatus("present frame",status);
        } catch(RuntimeException | Error failure) { throw failure; }
        catch(Throwable failure) { throw new IllegalStateException("present frame",failure); }
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
