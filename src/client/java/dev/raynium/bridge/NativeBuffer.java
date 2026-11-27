package dev.raynium.bridge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import dev.raynium.diagnostics.RayniumTelemetry;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.lwjgl.system.MemoryUtil;

public final class NativeBuffer implements GpuBuffer {
    public enum WriteSource { HOST_SHADOW,
                              TRANSIENT,
                              MAPPABLE_RING,
                              STAGED_VERTEX }
    @FunctionalInterface
    public interface StagingWriter {
        void writeBuffer(NativeBuffer target, long offset, MemorySegment source);
    }
    private static final Runnable NOOP_CLOSE = () -> {};
    private final WriteSource writeSource;
    private final NativeRenderer owner;
    private final StagingWriter stagingWriter;
    private final long handle;
    private final int usage;
    private final long size, address;
    private final ArrayList<OpenView> openWrites = new ArrayList<>();
    private final ArrayList<StagingView> openStaging = new ArrayList<>();
    private final Arena bufferArena;
    private final MemorySegment base;
    private boolean trackedDirty;
    private long[] ranges;
    private int rangeCount;
    private boolean closed, released;
    private int references;
    private long lastRetainedTapeId;

    static NativeBuffer create(NativeRenderer owner, MemorySegment renderer, int usage, long size, ByteBuffer initial, StagingWriter stagingWriter, WriteSource source) {
        if (size <= 0 || (usage & ~2047) != 0 || (usage & ~USAGE_HINT_CLIENT_STORAGE) == 0 || (initial != null && initial.remaining() != size))
            throw new IllegalArgumentException("Invalid buffer descriptor");
        if ((usage & USAGE_MAP_READ) != 0 && (usage & ~(USAGE_MAP_READ | USAGE_COPY_DST | USAGE_HINT_CLIENT_STORAGE)) != 0)
            throw new UnsupportedOperationException("MAP_READ buffer cannot have GPU read or write mapping usages");
        if (initial != null && !initial.isDirect()) {
            try (var scratch = Arena.ofConfined()) {
                var copied = scratch.allocate(size, 1);
                copied.copyFrom(MemorySegment.ofBuffer(initial));
                return create(owner, renderer, usage, size, copied, stagingWriter, source);
            }
        }
        return create(owner, renderer, usage, size, initial == null ? MemorySegment.NULL : MemorySegment.ofBuffer(initial), stagingWriter, source);
    }
    private static NativeBuffer create(NativeRenderer owner, MemorySegment renderer, int usage, long size, MemorySegment initial, StagingWriter stagingWriter, WriteSource source) {
        var desc = owner.bufferDesc;
        desc.set(JAVA_INT, 0, 24);
        desc.set(JAVA_INT, 4, usage);
        desc.set(JAVA_LONG, 8, size);
        desc.set(ADDRESS, 16, initial);
        var result = owner.bufferResult;
        NativeBindings.call(owner.bindings.createBuffer, "create buffer", renderer, desc, result);
        return new NativeBuffer(owner, stagingWriter, result.get(JAVA_LONG, 0), usage, size, result.get(ADDRESS, 8).address(), source);
    }
    private NativeBuffer(NativeRenderer owner, StagingWriter stagingWriter, long handle, int usage, long size, long address, WriteSource source) {
        this.writeSource = source;
        this.owner = owner;
        this.stagingWriter = stagingWriter;
        this.handle = handle;
        this.usage = usage & 1023;
        this.size = size;
        this.address = address;
        bufferArena = address == 0 ? null : Arena.ofConfined();
        base = address == 0 ? null : MemorySegment.ofAddress(address).reinterpret(size, bufferArena, null);
    }
    @Override
    public boolean isClosed() {
        return closed;
    }
    @Override
    public int usage() {
        return usage;
    }
    @Override
    public long size() {
        return size;
    }
    public long handle() {
        return handle;
    }
    public void checkRange(NativeRenderer renderer, long offset, long length, int requiredUsage) {
        owner.checkOpen();
        checkLiveRange(renderer, offset, length, requiredUsage);
    }
    private void checkLiveRange(NativeRenderer renderer, long offset, long length, int requiredUsage) {
        if (closed || owner != renderer)
            throw new IllegalStateException("Buffer is closed or belongs to another device");
        if ((usage & requiredUsage) != requiredUsage)
            throw new IllegalArgumentException("Missing buffer usage");
        if (offset < 0 || offset > size || length <= 0 || length > size - offset)
            throw new IllegalArgumentException("Invalid buffer range");
    }
    public long lastRetainedTapeId() {
        return lastRetainedTapeId;
    }
    public void setLastRetainedTapeId(long tapeId) {
        lastRetainedTapeId = tapeId;
    }
    public void retainRecording() {
        ++references;
    }
    public void releaseRecording() {
        if (references <= 0)
            throw new IllegalStateException("Unbalanced buffer reference");
        --references;
        retire();
    }
    private void retire() {
        if (closed && references == 0 && !released) {
            if (bufferArena != null)
                bufferArena.close();
            owner.destroyBuffer(handle);
            owner.bufferReleased(this);
            released = true;
        }
    }
    @Override
    public void close() {
        owner.checkThread();
        closed = true;
        retire();
    }
    void endRenderer() {
        closed = released = true;
        for (var staging : openStaging)
            staging.abort();
        openStaging.clear();
        for (var view : openWrites)
            view.closed = true;
        openWrites.clear();
        if (bufferArena != null)
            bufferArena.close();
    }
    @Override
    public GpuBufferSlice.MappedView map(long offset, long length, boolean read, boolean write) {
        if (read == write || length > Integer.MAX_VALUE)
            throw new IllegalArgumentException("Invalid buffer mapping");
        if (read) {
            owner.checkAlive();
            checkLiveRange(owner, offset, length, USAGE_MAP_READ);
            if (!owner.isPoisoned())
                owner.pollCompleted();
        } else
            checkRange(owner, offset, length, USAGE_MAP_WRITE);
        if (address != 0) {
            var bytes = MemoryUtil.memByteBuffer(address + offset, (int)length);
            if (read)
                bytes = bytes.asReadOnlyBuffer();
            bytes.order(ByteOrder.nativeOrder());
            var open = new OpenView(offset, length, write);
            if (write) {
                openWrites.add(open);
                trackRange(offset, length);
            }
            retainRecording();
            return new GpuBufferSlice.MappedView(new GpuBufferSlice(this, offset, length), bytes, open);
        }
        var staging = new StagingView(offset, length);
        openStaging.add(staging);
        retainRecording();
        return new GpuBufferSlice.MappedView(new GpuBufferSlice(this, offset, length), staging.bytes(), staging);
    }

    /** Maps a transient fence-rotated slice directly without per-view tracking; dirty range is recorded immediately. */
    public GpuBufferSlice.MappedView mapTransient(long offset, long length) {
        if (length > Integer.MAX_VALUE)
            throw new IllegalArgumentException("Invalid buffer mapping");
        checkRange(owner, offset, length, USAGE_MAP_WRITE);
        if (address == 0 || writeSource != WriteSource.TRANSIENT)
            throw new IllegalArgumentException("Invalid transient mapping");
        trackRange(offset, length);
        var bytes = MemoryUtil.memByteBuffer(address + offset, (int)length).order(ByteOrder.nativeOrder());
        return new GpuBufferSlice.MappedView(new GpuBufferSlice(this, offset, length), bytes, NOOP_CLOSE);
    }

    /** Fills a fence-rotated slice, including partial allocator grants. Source positions are preserved. */
    public void writePersistent(long offset, long length, List<ByteBuffer> chunks, long alignment) {
        checkRange(owner, offset, length, USAGE_MAP_WRITE);
        if (base == null || writeSource != WriteSource.TRANSIENT || alignment <= 0)
            throw new IllegalArgumentException("Invalid persistent upload");
        long cursor = 0;
        for (var chunk : chunks) {
            if (cursor >= length)
                break;
            long count = Math.min(chunk.remaining(), length - cursor);
            MemorySegment.copy(MemorySegment.ofBuffer(chunk), 0, base, offset + cursor, count);
            cursor += count;
            cursor += Math.min((alignment - cursor % alignment) % alignment, length - cursor);
        }
        trackRange(offset, length);
    }
    private void trackRange(long offset, long length) {
        addRange(offset, length);
        if (!trackedDirty) {
            trackedDirty = true;
            retainRecording();
            owner.trackWritable(this);
        }
    }

    private void addRange(long start, long length) {
        long end = start + length;
        if (ranges == null)
            ranges = new long[4];
        int first = 0;
        while (first < rangeCount && ranges[first * 2 + 1] < start)
            ++first;
        int after = first;
        while (after < rangeCount && ranges[after * 2] <= end) {
            start = Math.min(start, ranges[after * 2]);
            end = Math.max(end, ranges[after * 2 + 1]);
            ++after;
        }
        int count = rangeCount - (after - first) + 1;
        if (count * 2 > ranges.length)
            ranges = Arrays.copyOf(ranges, ranges.length * 2);
        System.arraycopy(ranges, 2 * after, ranges, 2 * (first + 1), 2 * (rangeCount - after));
        ranges[2 * first] = start;
        ranges[2 * first + 1] = end;
        rangeCount = count;
    }
    int rangeCount() {
        return rangeCount;
    }
    long encodeRanges(MemorySegment dirty, long at) {
        for (int i = 0; i < rangeCount; i++, at += 24) {
            dirty.set(JAVA_LONG, at, handle);
            dirty.set(JAVA_LONG, at + 8, ranges[2 * i]);
            dirty.set(JAVA_LONG, at + 16, ranges[2 * i + 1] - ranges[2 * i]);
        }
        return at;
    }
    void measureWrites() {
        long bytes = 0;
        for (int i = 0; i < rangeCount; ++i)
            bytes += ranges[2 * i + 1] - ranges[2 * i];
        RayniumTelemetry.onMappedWrites(writeSource.ordinal(), bytes);
    }
    boolean afterSubmit() {
        rangeCount = 0;
        for (int i = 0; i < openWrites.size(); i++) {
            var view = openWrites.get(i);
            addRange(view.offset, view.length);
        }
        if (rangeCount != 0)
            return false;
        if (ranges != null && ranges.length > 1024)
            ranges = null;
        trackedDirty = false;
        releaseRecording();
        return true;
    }

    private final class OpenView implements Runnable {
        final long offset, length;
        final boolean write;
        boolean closed;
        OpenView(long offset, long length, boolean write) {
            this.offset = offset;
            this.length = length;
            this.write = write;
        }
        @Override
        public void run() {
            owner.checkThread();
            if (closed)
                return;
            closed = true;
            if (write)
                openWrites.remove(this);
            releaseRecording();
        }
    }

    private final class StagingView implements Runnable {
        private Arena arena;
        private MemorySegment memory;
        private final long offset;
        StagingView(long offset, long length) {
            this.offset = offset;
            this.arena = Arena.ofConfined();
            try {
                this.memory = arena.allocate(length, 8);
            } catch (Throwable failure) {
                abort();
                throw failure;
            }
        }
        ByteBuffer bytes() {
            return memory.asByteBuffer().order(ByteOrder.nativeOrder());
        }
        @Override
        public void run() {
            owner.checkThread();
            if (arena == null)
                return;
            try {
                if (!closed)
                    stagingWriter.writeBuffer(NativeBuffer.this, offset, memory);
            } finally {
                abort();
                openStaging.remove(this);
                releaseRecording();
            }
        }
        void abort() {
            if (arena != null) {
                arena.close();
                arena = null;
                memory = null;
            }
        }
    }
}
