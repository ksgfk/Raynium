package dev.raynium.bridge;

import static java.lang.foreign.ValueLayout.JAVA_DOUBLE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.MemorySegment;

/** Immutable information about the actual native adapter; no Minecraft device is involved. */
public record NativeDeviceInfo(int backend, int deviceType, int vendorId, long uniformOffsetAlignment,
                               int maxTextureSize, int maxColorAttachments, float maxAnisotropy, int flags,
                               String name, String vendor, String driver, TransferLimits transfer, TimestampCalibration timestamps) {
    public record TransferLimits(long maxBufferSize, long maxUniformRange, long maxStorageRange, long storageAlignment, long texelAlignment,
                                 long bufferCopyAlignment, long texturePlacementAlignment, int texturePitchAlignment, int maxTexture1D,
                                 int maxTexture3D, int maxArrayLayers, int maxVertexBindings, int maxPushConstantBytes, long maxIndirectDrawCount, long hostCoherentAtomSize) {}
    public record TimestampCalibration(int validBits, long frequency, double tickPeriodNs) {}
    static final int BYTE_SIZE = 600;
    private static long capacity(MemorySegment info, long offset) {
        long value = info.get(JAVA_LONG, offset);
        return value < 0 ? Long.MAX_VALUE : value;
    }
    static NativeDeviceInfo read(MemorySegment info) {
        if (info.get(JAVA_INT, 0) != BYTE_SIZE)
            throw new IllegalStateException("Device info layout mismatch");
        return new NativeDeviceInfo(info.get(JAVA_INT, 4), info.get(JAVA_INT, 8), info.get(JAVA_INT, 12),
                                    info.get(JAVA_LONG, 16), info.get(JAVA_INT, 24), info.get(JAVA_INT, 28), info.get(JAVA_FLOAT, 32), info.get(JAVA_INT, 36),
                                    info.asSlice(40, 256).getString(0), info.asSlice(296, 64).getString(0), info.asSlice(360, 128).getString(0),
                                    new TransferLimits(capacity(info, 488), capacity(info, 496), capacity(info, 504), info.get(JAVA_LONG, 512),
                                                       info.get(JAVA_LONG, 520), info.get(JAVA_LONG, 528), info.get(JAVA_LONG, 536), info.get(JAVA_INT, 544), info.get(JAVA_INT, 548),
                                                       info.get(JAVA_INT, 552), info.get(JAVA_INT, 556), info.get(JAVA_INT, 560), info.get(JAVA_INT, 564), Integer.toUnsignedLong(info.get(JAVA_INT, 568)), info.get(JAVA_LONG, 592)),
                                    new TimestampCalibration(info.get(JAVA_INT, 572), info.get(JAVA_LONG, 576), info.get(JAVA_DOUBLE, 584)));
    }
    public boolean wireframe() {
        return (flags & 1) != 0;
    }
}
