package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

final class NativeBindings {
    record Probe(int abiVersion, int vertexStride, int allocatorMask) {}

    private final MethodHandle version;
    private final MethodHandle query;
    private final MethodHandle vertexStride;
    private final MethodHandle allocator;

    NativeBindings(SymbolLookup library) {
        Linker linker = Linker.nativeLinker();
        FunctionDescriptor scalar = FunctionDescriptor.of(ValueLayout.JAVA_INT);
        version = bind(linker, library, "rn_get_abi_version", scalar);
        query = bind(linker, library, "rn_query_abi", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        vertexStride = bind(linker, library, "rn_probe_vertex_stride", scalar);
        allocator = bind(linker, library, "rn_probe_allocator", scalar);
    }

    Probe inspect(NativeManifest expected) {
        try (Arena scratch = Arena.ofConfined()) {
            int abi = (int) version.invokeExact();
            if (abi != expected.abiVersion()) throw new IllegalStateException("Native ABI version mismatch: " + abi);
            MemorySegment info = scratch.allocate(240, 4);
            int status = (int) query.invokeExact(info, 240);
            if (status != 0) throw new IllegalStateException("rn_query_abi failed with status " + status);
            int flags = 6 | (expected.radrayDirty() ? 1 : 0);
            if (info.get(ValueLayout.JAVA_INT, 0) != expected.abiInfoSize()
                    || info.get(ValueLayout.JAVA_INT, 4) != abi
                    || info.get(ValueLayout.JAVA_INT, 8) != 8
                    || info.get(ValueLayout.JAVA_INT, 12) != flags
                    || !text(info, 16, 72).equals(expected.buildId())
                    || !text(info, 88, 48).equals(expected.radrayCommit())
                    || !text(info, 136, 16).equals(expected.configuration())
                    || !text(info, 152, 24).equals(expected.platform())
                    || !text(info, 176, 64).equals(expected.toolchain())) {
                throw new IllegalStateException("Loaded DLL build identity/layout does not match native-manifest.json");
            }
            int stride = (int) vertexStride.invokeExact();
            int mask = (int) allocator.invokeExact();
            if (stride != 12) throw new IllegalStateException("RadRay vertex stride probe failed: " + stride);
            if (mask != 7) throw new IllegalStateException("DLL allocation override probe failed: expected mask 7, got " + mask);
            return new Probe(abi, stride, mask);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            // MethodHandle.invokeExact declares Throwable even for these non-throwing C functions.
            throw new IllegalStateException("Cannot invoke the Raynium C ABI", failure);
        }
    }

    private MethodHandle bind(Linker linker, SymbolLookup library, String name, FunctionDescriptor descriptor) {
        MemorySegment address = library.find(name).orElseThrow(() -> new IllegalStateException("Native symbol is missing: " + name));
        return linker.downcallHandle(address, descriptor);
    }

    private String text(MemorySegment info, long offset, long capacity) {
        return info.asSlice(offset, capacity).getString(0);
    }
}
