package dev.raynium.bridge;

import java.io.IOException;
import java.io.InputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

/** Immutable CPU asset; native creation borrows a temporary copy for one call. */
public final class NativeShaderPack {
    public static final int MAX_SIZE = 64 << 20;
    private final byte[] bytes;

    private NativeShaderPack(byte[] bytes) { this.bytes = bytes; }

    public static NativeShaderPack fromBytes(byte[] bytes) {
        if (bytes.length == 0 || bytes.length > MAX_SIZE) throw new IllegalArgumentException("Invalid shader pack size");
        return new NativeShaderPack(bytes.clone());
    }

    public static NativeShaderPack read(InputStream input) throws IOException {
        byte[] bytes = input.readNBytes(MAX_SIZE + 1);
        if (bytes.length == 0 || bytes.length > MAX_SIZE) throw new IOException("Invalid shader pack size");
        return new NativeShaderPack(bytes);
    }

    int size() { return bytes.length; }
    MemorySegment copyTo(Arena arena) {
        var copy = arena.allocate(bytes.length, 8);
        MemorySegment.copy(bytes, 0, copy, java.lang.foreign.ValueLayout.JAVA_BYTE, 0, bytes.length);
        return copy;
    }
}
