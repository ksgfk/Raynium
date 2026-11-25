package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.commands.GpuFence;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class BatchSubmissionTest {
    @Test void recordingAndFailedFencesNeverEnterNativeWait() {
        var batch = new BatchSubmission((seq, timeout) -> fail("Unsubmitted native wait"));
        try (var fence = batch.createFence()) {
            assertFalse(fence.awaitCompletion(0));
            assertThrows(IllegalStateException.class, () -> fence.awaitCompletion(GpuFence.NO_TIMEOUT));
            batch.failed();
            assertFalse(fence.awaitCompletion(0));
            assertThrows(IllegalStateException.class, () -> fence.awaitCompletion(1));
            assertThrows(IllegalStateException.class, () -> batch.enqueued(1));
        }
    }

    @Test void olderFenceWaitsForOriginalSequenceEvenAfterNextSubmission() {
        var calls = new ArrayList<Long>();
        BatchSubmission.Waiter wait = (seq, timeout) -> { calls.add(seq); calls.add(timeout); return true; };
        var first = new BatchSubmission(wait);
        var next = new BatchSubmission(wait);
        try (var fence = first.createFence(); var later = next.createFence()) {
            first.enqueued(2);
            next.enqueued(7);
            assertTrue(later.awaitCompletion(Long.MAX_VALUE));
            assertTrue(fence.awaitCompletion(0));
            assertEquals(List.of(7L, Long.MAX_VALUE, 2L, 0L), calls);
        }
    }
}
