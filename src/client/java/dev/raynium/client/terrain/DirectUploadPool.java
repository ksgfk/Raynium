package dev.raynium.client.terrain;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Lock-free power-of-two pool of native-order DirectByteBuffers for terrain vertex and index uploads.
 * Avoids Arena.ofShared() close safepoints and eliminates Worker-Main heap byte[]/int[] GC pressure.
 */
public final class DirectUploadPool {
    static final int MIN_SHIFT = 12; // 4 KiB
    static final int MAX_SHIFT = 23; // 8 MiB
    static final int MIN_BUCKET_BYTES = 1 << MIN_SHIFT;
    static final int MAX_BUCKET_BYTES = 1 << MAX_SHIFT;
    static final long MAX_POOLED_BYTES = 64L * 1024 * 1024;
    private static final ByteBuffer EMPTY = ByteBuffer.allocateDirect(0).order(ByteOrder.nativeOrder());
    @SuppressWarnings("unchecked")
    private final ConcurrentLinkedQueue<ByteBuffer>[] buckets = new ConcurrentLinkedQueue[MAX_SHIFT - MIN_SHIFT + 1];
    private final AtomicLong pooledBytes = new AtomicLong();

    private static volatile DirectUploadPool current = new DirectUploadPool(1);
    private final long epoch;
    private DirectUploadPool(long epoch) {
        this.epoch = epoch;
        for (int i = 0; i < buckets.length; i++) {
            buckets[i] = new ConcurrentLinkedQueue<>();
        }
    }

    static void retire(long epoch) {
        current = new DirectUploadPool(epoch);
    }

    static int bucketIndex(int requiredBytes) {
        if (requiredBytes <= MIN_BUCKET_BYTES)
            return 0;
        int ceilShift = 32 - Integer.numberOfLeadingZeros(requiredBytes - 1);
        return ceilShift - MIN_SHIFT;
    }

    static ByteBuffer acquire(int requiredBytes, long epoch) {
        var pool = current;
        if (requiredBytes < 0)
            throw new IllegalArgumentException("Negative direct upload buffer size: " + requiredBytes);
        if (requiredBytes == 0)
            return EMPTY;
        if (requiredBytes > MAX_BUCKET_BYTES || epoch != pool.epoch) {
            return ByteBuffer.allocateDirect(requiredBytes).order(ByteOrder.nativeOrder());
        }
        int bucket = bucketIndex(requiredBytes);
        ByteBuffer pooled = pool.buckets[bucket].poll();
        if (pooled != null) {
            pool.pooledBytes.addAndGet(-pooled.capacity());
            pooled.clear();
            pooled.limit(requiredBytes);
            return pooled;
        }
        int capacity = 1 << (MIN_SHIFT + bucket);
        ByteBuffer fresh = ByteBuffer.allocateDirect(capacity).order(ByteOrder.nativeOrder());
        fresh.limit(requiredBytes);
        return fresh;
    }

    static void release(ByteBuffer buffer, long epoch) {
        var pool = current;
        if (epoch != pool.epoch)
            return;
        if (buffer == null || !buffer.isDirect() || buffer.isReadOnly())
            return;
        int cap = buffer.capacity();
        if (cap < MIN_BUCKET_BYTES || cap > MAX_BUCKET_BYTES || (cap & (cap - 1)) != 0)
            return;
        while (true) {
            long used = pool.pooledBytes.get();
            if (used + cap > MAX_POOLED_BYTES)
                return;
            if (pool.pooledBytes.compareAndSet(used, used + cap)) {
                buffer.clear();
                pool.buckets[Integer.numberOfTrailingZeros(cap) - MIN_SHIFT].offer(buffer);
                return;
            }
        }
    }

    static long pooledBytes() {
        return current.pooledBytes.get();
    }

    static void clearForTests() {
        retire(TerrainCapture.epoch());
    }
}
