package dev.raynium.bridge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.textures.GpuTexture;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Objects;
/** Renderer-owned texture. Upload copies caller bytes before returning; IDs are never reused. */
public final class NativeTexture implements GpuTexture {
    public enum Format {
        RGBA8(GpuFormat.RGBA8_UNORM),
        R8(GpuFormat.R8_UNORM),
        RGBA16F(GpuFormat.RGBA16_FLOAT),
        RGBA32F(GpuFormat.RGBA32_FLOAT),
        DEPTH32F(GpuFormat.D32_FLOAT),
        R8_SNORM(GpuFormat.R8_SNORM),
        RG8_UNORM(GpuFormat.RG8_UNORM),
        RG8_SNORM(GpuFormat.RG8_SNORM),
        RGBA8_SNORM(GpuFormat.RGBA8_SNORM),
        R16_UNORM(GpuFormat.R16_UNORM),
        R16_SNORM(GpuFormat.R16_SNORM),
        RG16_UNORM(GpuFormat.RG16_UNORM),
        RG16_SNORM(GpuFormat.RG16_SNORM),
        RGBA16_UNORM(GpuFormat.RGBA16_UNORM),
        RGBA16_SNORM(GpuFormat.RGBA16_SNORM),
        R8_UINT(GpuFormat.R8_UINT),
        R8_SINT(GpuFormat.R8_SINT),
        RG8_UINT(GpuFormat.RG8_UINT),
        RG8_SINT(GpuFormat.RG8_SINT),
        RGBA8_UINT(GpuFormat.RGBA8_UINT),
        RGBA8_SINT(GpuFormat.RGBA8_SINT),
        R16_UINT(GpuFormat.R16_UINT),
        R16_SINT(GpuFormat.R16_SINT),
        RG16_UINT(GpuFormat.RG16_UINT),
        RG16_SINT(GpuFormat.RG16_SINT),
        RGBA16_UINT(GpuFormat.RGBA16_UINT),
        RGBA16_SINT(GpuFormat.RGBA16_SINT),
        R32_UINT(GpuFormat.R32_UINT),
        R32_SINT(GpuFormat.R32_SINT),
        RG32_UINT(GpuFormat.RG32_UINT),
        RG32_SINT(GpuFormat.RG32_SINT),
        RGBA32_UINT(GpuFormat.RGBA32_UINT),
        RGBA32_SINT(GpuFormat.RGBA32_SINT),
        R16_FLOAT(GpuFormat.R16_FLOAT),
        RG16_FLOAT(GpuFormat.RG16_FLOAT),
        R32_FLOAT(GpuFormat.R32_FLOAT),
        RG32_FLOAT(GpuFormat.RG32_FLOAT),
        RGB10A2_UNORM(GpuFormat.RGB10A2_UNORM),
        RGB10A2_UINT(GpuFormat.RGB10A2_UINT),
        RG11B10_FLOAT(GpuFormat.RG11B10_FLOAT),
        D32_FLOAT_S8_UINT(GpuFormat.D32_FLOAT_S8_UINT),
        D24_UNORM_S8_UINT(GpuFormat.D24_UNORM_S8_UINT),
        D16_UNORM(GpuFormat.D16_UNORM);
        private final GpuFormat gpu;
        Format(GpuFormat gpu) {
            this.gpu = gpu;
        }
        public GpuFormat gpu() {
            return gpu;
        }
        public int bytesPerPixel() {
            return gpu.blockSize();
        }
        public static Format from(GpuFormat gpu) {
            for (var format : values())
                if (format.gpu == gpu)
                    return format;
            throw new UnsupportedOperationException("Unsupported texture format " + gpu);
        }
        public boolean supportsFloatBlit() {
            if (!gpu.hasColorAspect() || gpu == GpuFormat.RGB10A2_UINT)
                return false;
            return switch (gpu.componentType()) {
                case UINT_8, UINT_16, UINT_32, SINT_8, SINT_16, SINT_32 -> false;
                default -> true;
            };
        }
    }
    public static final int CUBE = 1, ATTACHMENT = 2, READBACK = 4;
    private final NativeRenderer owner;
    private final long handle;
    private final int width, height, mips, layers, flags;
    private int recordingReferences;
    private long lastRetainedTapeId;
    private final Format format;
    private final String label;
    private final int usage;
    private boolean closed;
    NativeTexture(NativeRenderer owner, long handle, int width, int height, int mips, int layers, Format format, int flags, int usage, String label) {
        this.owner = owner;
        this.handle = handle;
        this.width = width;
        this.height = height;
        this.mips = mips;
        this.layers = layers;
        this.format = format;
        this.flags = flags;
        this.usage = usage;
        this.label = label;
    }
    @Override
    public String getLabel() {
        return label;
    }
    @Override
    public int usage() {
        return usage;
    }
    @Override
    public boolean isClosed() {
        return closed;
    }
    @Override
    public GpuFormat getFormat() {
        return format.gpu();
    }
    @Override
    public int getWidth(int mip) {
        Objects.checkIndex(mip, mips);
        return Math.max(1, width >> mip);
    }
    @Override
    public int getHeight(int mip) {
        Objects.checkIndex(mip, mips);
        return Math.max(1, height >> mip);
    }
    @Override
    public int getDepthOrLayers() {
        return layers;
    }
    @Override
    public int getMipLevels() {
        return mips;
    }
    public int flags() {
        return flags;
    }
    public void checkRecordingOwner(NativeRenderer renderer) {
        checkOpen();
        if (owner != renderer)
            throw new IllegalArgumentException("Texture belongs to a different renderer");
    }
    public long lastRetainedTapeId() {
        return lastRetainedTapeId;
    }
    public void setLastRetainedTapeId(long tapeId) {
        lastRetainedTapeId = tapeId;
    }
    public void retainRecording() {
        checkOpen();
        recordingReferences = Math.incrementExact(recordingReferences);
    }
    public void releaseRecording() {
        owner.checkThread();
        if (recordingReferences <= 0)
            throw new IllegalStateException("Unbalanced texture recording reference");
        if (--recordingReferences == 0 && closed && !owner.isClosed())
            owner.destroyTexture(handle);
    }
    public long handle() {
        checkOpen();
        return handle;
    }
    public int width() {
        return width;
    }
    public int height() {
        return height;
    }
    public int mipLevels() {
        return mips;
    }
    public int arrayLayers() {
        return layers;
    }
    public Format format() {
        return format;
    }
    public void upload(int mip, int x, int y, int width, int height, int rowPitch, MemorySegment pixels) {
        uploadLayer(mip, 0, x, y, width, height, rowPitch, pixels);
    }
    public void uploadLayer(int mip, int layer, int x, int y, int width, int height, int rowPitch, MemorySegment pixels) {
        checkOpen();
        try (var arena = Arena.ofConfined()) {
            if (!pixels.isNative()) {
                var copied = arena.allocate(pixels.byteSize(), 1);
                copied.copyFrom(pixels);
                pixels = copied;
            }
            var desc = arena.allocate(56, 8);
            desc.set(JAVA_INT, 0, 56);
            desc.set(JAVA_INT, 4, mip);
            desc.set(JAVA_LONG, 8, handle);
            desc.set(JAVA_INT, 16, x);
            desc.set(JAVA_INT, 20, y);
            desc.set(JAVA_INT, 24, width);
            desc.set(JAVA_INT, 28, height);
            desc.set(JAVA_INT, 32, rowPitch);
            desc.set(JAVA_INT, 36, layer);
            desc.set(ADDRESS, 40, pixels);
            desc.set(JAVA_LONG, 48, pixels.byteSize());
            owner.uploadTexture(desc);
        }
    }
    public void upload(int mip, int x, int y, int width, int height, byte[] pixels) {
        upload(mip, x, y, width, height, Math.multiplyExact(width, format.bytesPerPixel()), MemorySegment.ofArray(pixels));
    }
    private void checkOpen() {
        owner.checkOpen();
        if (closed)
            throw new IllegalStateException("Texture is closed");
    }
    @Override
    public void close() {
        owner.checkThread();
        if (closed)
            return;
        if (recordingReferences == 0 && !owner.isClosed())
            owner.destroyTexture(handle);
        closed = true;
    }
}
