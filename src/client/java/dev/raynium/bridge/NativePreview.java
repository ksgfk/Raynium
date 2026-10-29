package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

/** Consumer GPU use and imports must finish before close; failed close keeps the owner alive. */
public final class NativePreview implements AutoCloseable {
    public record Info(int handleType, int width, int height, long memoryHandle, long readyHandle,
                       long consumedHandle, long allocationSize, int memoryTypeIndex, int nodeMask,
                       byte[] luid, byte[] deviceUuid, byte[] driverUuid, int nativeFormat,
                       int nativeUsage, int nativeFlags, int nativeTiling, long allocationOffset, boolean dedicated) {
        public Info {
            luid = luid.clone(); deviceUuid = deviceUuid.clone(); driverUuid = driverUuid.clone();
        }
        @Override public byte[] luid() { return luid.clone(); }
        @Override public byte[] deviceUuid() { return deviceUuid.clone(); }
        @Override public byte[] driverUuid() { return driverUuid.clone(); }

        static Info read(MemorySegment s) {
            if (s.get(ValueLayout.JAVA_INT, 0) != 128) throw new IllegalStateException("Preview info layout mismatch");
            return new Info(s.get(ValueLayout.JAVA_INT, 4), s.get(ValueLayout.JAVA_INT, 8), s.get(ValueLayout.JAVA_INT, 12),
                    s.get(ValueLayout.JAVA_LONG, 16), s.get(ValueLayout.JAVA_LONG, 24), s.get(ValueLayout.JAVA_LONG, 32),
                    s.get(ValueLayout.JAVA_LONG, 40), s.get(ValueLayout.JAVA_INT, 48), s.get(ValueLayout.JAVA_INT, 52),
                    s.asSlice(56, 8).toArray(ValueLayout.JAVA_BYTE), s.asSlice(64, 16).toArray(ValueLayout.JAVA_BYTE),
                    s.asSlice(80, 16).toArray(ValueLayout.JAVA_BYTE), s.get(ValueLayout.JAVA_INT, 96),
                    s.get(ValueLayout.JAVA_INT, 100), s.get(ValueLayout.JAVA_INT, 104), s.get(ValueLayout.JAVA_INT, 108),
                    s.get(ValueLayout.JAVA_LONG, 112), s.get(ValueLayout.JAVA_INT, 120) == 1);
        }
    }

    private final NativeRuntime owner;
    private final NativeBindings bindings;
    private final Info info;
    private MemorySegment handle;

    NativePreview(NativeRuntime owner, NativeBindings bindings, MemorySegment handle, Info info) {
        this.owner = owner; this.bindings = bindings; this.handle = handle; this.info = info;
    }

    public Info info() { checkOpen(); return info; }

    public void draw(long sequence) {
        checkOpen();
        bindings.drawPreview(handle, sequence);
    }

    MemorySegment handle() { checkOpen(); return handle; }

    private void checkOpen() {
        owner.checkOpen();
        if (handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Preview is closed");
    }

    @Override public void close() {
        owner.checkThread();
        if (handle.equals(MemorySegment.NULL)) return;
        bindings.destroyPreview(handle);
        handle = MemorySegment.NULL;
        owner.releasedPreview(this);
    }
}
