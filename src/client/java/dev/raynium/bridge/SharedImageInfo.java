package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

public record SharedImageInfo(int handleType, int width, int height, long memoryHandle, long readyHandle,
                   long consumedHandle, long allocationSize, int memoryTypeIndex, int nodeMask,
                   byte[] luid, byte[] deviceUuid, byte[] driverUuid, int nativeFormat,
                   int nativeUsage, int nativeFlags, int nativeTiling, long allocationOffset, boolean dedicated) {
    public SharedImageInfo {
        luid = luid.clone(); deviceUuid = deviceUuid.clone(); driverUuid = driverUuid.clone();
    }
    @Override public byte[] luid() { return luid.clone(); }
    @Override public byte[] deviceUuid() { return deviceUuid.clone(); }
    @Override public byte[] driverUuid() { return driverUuid.clone(); }

    static SharedImageInfo read(MemorySegment s) {
        if (s.get(ValueLayout.JAVA_INT, 0) != 128) throw new IllegalStateException("Shared image info layout mismatch");
        return new SharedImageInfo(s.get(ValueLayout.JAVA_INT, 4), s.get(ValueLayout.JAVA_INT, 8), s.get(ValueLayout.JAVA_INT, 12),
                s.get(ValueLayout.JAVA_LONG, 16), s.get(ValueLayout.JAVA_LONG, 24), s.get(ValueLayout.JAVA_LONG, 32),
                s.get(ValueLayout.JAVA_LONG, 40), s.get(ValueLayout.JAVA_INT, 48), s.get(ValueLayout.JAVA_INT, 52),
                s.asSlice(56, 8).toArray(ValueLayout.JAVA_BYTE), s.asSlice(64, 16).toArray(ValueLayout.JAVA_BYTE),
                s.asSlice(80, 16).toArray(ValueLayout.JAVA_BYTE), s.get(ValueLayout.JAVA_INT, 96),
                s.get(ValueLayout.JAVA_INT, 100), s.get(ValueLayout.JAVA_INT, 104), s.get(ValueLayout.JAVA_INT, 108),
                s.get(ValueLayout.JAVA_LONG, 112), s.get(ValueLayout.JAVA_INT, 120) == 1);
    }
}

