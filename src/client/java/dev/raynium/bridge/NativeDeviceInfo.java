package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import static java.lang.foreign.ValueLayout.*;

/** Immutable information about the actual native adapter; no Minecraft device is involved. */
public record NativeDeviceInfo(int backend,int deviceType,int vendorId,long uniformOffsetAlignment,
                               int maxTextureSize,int maxColorAttachments,float maxAnisotropy,
                               String name,String vendor,String driver) {
    static final int BYTE_SIZE=488;
    static NativeDeviceInfo read(MemorySegment info) {
        if(info.get(JAVA_INT,0)!=BYTE_SIZE) throw new IllegalStateException("Device info layout mismatch");
        return new NativeDeviceInfo(info.get(JAVA_INT,4),info.get(JAVA_INT,8),info.get(JAVA_INT,12),
            info.get(JAVA_LONG,16),info.get(JAVA_INT,24),info.get(JAVA_INT,28),info.get(JAVA_FLOAT,32),
            info.asSlice(40,256).getString(0),info.asSlice(296,64).getString(0),info.asSlice(360,128).getString(0));
    }
}
