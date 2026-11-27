package dev.raynium.diagnostics;

/** Zero-allocation static mount point for optional runtime instrumentation. */
public final class RayniumTelemetry {
    public interface TelemetryListener {
        default boolean isMeasuring() {
            return false;
        }

        default void onMappedWrites(int writeSourceOrdinal, long bytes) {
        }

        default void onPresentedFrames(long count) {
        }

        default long syncWaitStart() {
            return 0L;
        }

        default void syncWaitEnd(long startedNs) {
        }

        default void onBatchSubmitted(Object renderer, long sequence, long javaPrepareNs, long nativeCallNs) {
        }

        default void onPipelineSelected(Object renderPipeline) {
        }
    }

    private static volatile TelemetryListener listener;

    private RayniumTelemetry() {
    }

    public static void setListener(TelemetryListener next) {
        listener = next;
    }

    public static boolean isMeasuring() {
        TelemetryListener current = listener;
        return current != null && current.isMeasuring();
    }

    public static void onMappedWrites(int writeSourceOrdinal, long bytes) {
        TelemetryListener current = listener;
        if (current != null) {
            current.onMappedWrites(writeSourceOrdinal, bytes);
        }
    }

    public static void onPresentedFrames(long count) {
        TelemetryListener current = listener;
        if (current != null) {
            current.onPresentedFrames(count);
        }
    }

    public static long syncWaitStart() {
        TelemetryListener current = listener;
        return current != null ? current.syncWaitStart() : 0L;
    }

    public static void syncWaitEnd(long startedNs) {
        TelemetryListener current = listener;
        if (current != null) {
            current.syncWaitEnd(startedNs);
        }
    }

    public static void onBatchSubmitted(Object renderer, long sequence, long javaPrepareNs, long nativeCallNs) {
        TelemetryListener current = listener;
        if (current != null) {
            current.onBatchSubmitted(renderer, sequence, javaPrepareNs, nativeCallNs);
        }
    }

    public static void onPipelineSelected(Object renderPipeline) {
        TelemetryListener current = listener;
        if (current != null) {
            current.onPipelineSelected(renderPipeline);
        }
    }
}
