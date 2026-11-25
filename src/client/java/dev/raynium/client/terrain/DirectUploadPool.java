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
    private static final ConcurrentLinkedQueue<ByteBuffer>[] BUCKETS = new ConcurrentLinkedQueue[MAX_SHIFT - MIN_SHIFT + 1];
    private static final AtomicLong POOLED_BYTES = new AtomicLong();

    static {
        for (int i = 0; i < BUCKETS.length; i++) {
            BUCKETS[i] = new ConcurrentLinkedQueue<>();
        }
    }

    private DirectUploadPool() {}

    static int bucketIndex(int requiredBytes) {
        if (requiredBytes <= MIN_BUCKET_BYTES) return 0;
        int ceilShift = 32 - Integer.numberOfLeadingZeros(requiredBytes - 1);
        return ceilShift - MIN_SHIFT;
    }

    public static ByteBuffer acquire(int requiredBytes) {
        if (requiredBytes < 0) throw new IllegalArgumentException("Negative direct upload buffer size: " + requiredBytes);
        if (requiredBytes == 0) return EMPTY;
        if (requiredBytes > MAX_BUCKET_BYTES) {
            return ByteBuffer.allocateDirect(requiredBytes).order(ByteOrder.nativeOrder());
        }
        int bucket = bucketIndex(requiredBytes);
        ByteBuffer pooled = BUCKETS[bucket].poll();
        if (pooled != null) {
            POOLED_BYTES.addAndGet(-pooled.capacity());
            pooled.clear();
            pooled.limit(requiredBytes);
            return pooled;
        }
        int capacity = 1 << (MIN_SHIFT + bucket);
        ByteBuffer fresh = ByteBuffer.allocateDirect(capacity).order(ByteOrder.nativeOrder());
        fresh.limit(requiredBytes);
        return fresh;
    }

    public static void release(ByteBuffer buffer) {
        if (buffer == null || !buffer.isDirect() || buffer.isReadOnly()) return;
        int cap = buffer.capacity();
        if (cap < MIN_BUCKET_BYTES || cap > MAX_BUCKET_BYTES || (cap & (cap - 1)) != 0) return;
        while (true) {
            long current = POOLED_BYTES.get();
            if (current + cap > MAX_POOLED_BYTES) return;
            if (POOLED_BYTES.compareAndSet(current, current + cap)) {
                buffer.clear();
                BUCKETS[Integer.numberOfTrailingZeros(cap) - MIN_SHIFT].offer(buffer);
                return;
            }
        }
    }

    static long pooledBytes() {
        return POOLED_BYTES.get();
    }

    static void clearForTests() {
        for (var q : BUCKETS) {
            ByteBuffer buf;
            while ((buf = q.poll()) != null) {
                POOLED_BYTES.addAndGet(-buf.capacity());
            }
        }
    }
}
