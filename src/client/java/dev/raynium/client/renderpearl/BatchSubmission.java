package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.commands.GpuFence;

/** A fence follows the specific recording batch from which it was created. */
final class BatchSubmission {
    interface Waiter {
        boolean awaitAndPublish(long sequence, long timeoutNs);
    }

    private enum State { RECORDING, ENQUEUED, FAILED }

    private final Waiter waiter;
    private State state = State.RECORDING;
    private long sequence;

    BatchSubmission(Waiter waiter) {
        this.waiter = waiter;
    }

    void enqueued(long sequence) {
        if (sequence <= 0 || state != State.RECORDING) throw new IllegalStateException("Invalid submission transition");
        this.sequence = sequence;
        state = State.ENQUEUED;
    }

    void failed() {
        sequence=0;
        state = State.FAILED;
    }

    long submittedSequence() { return sequence; }

    GpuFence createFence() {
        return new GpuFence() {
            private boolean closed;

            @Override public boolean awaitCompletion(long timeoutNs) {
                if (closed) throw new IllegalStateException("Fence is closed");
                if (state == State.ENQUEUED) return waiter.awaitAndPublish(sequence, timeoutNs);
                if (timeoutNs == 0) return false;
                throw new IllegalStateException("Cannot wait for a " + state + " batch");
            }

            @Override public void close() { closed = true; }
        };
    }
}
