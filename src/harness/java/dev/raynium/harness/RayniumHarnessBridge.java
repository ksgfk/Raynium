package dev.raynium.harness;

import java.lang.foreign.MemorySegment;
import java.util.function.Consumer;
import net.fabricmc.loader.api.FabricLoader;

/**
 * Optional runtime bridge to the Raynium core mod.
 * Keeps all core types isolated inside {@link RayniumCoreBridge} so pure vanilla reference sessions
 * never load core classes when the {@code raynium} mod is absent.
 */
public final class RayniumHarnessBridge {
    public interface BenchmarkCallbacks {
        boolean isMeasuring();
        void onMappedWrites(int writeSourceOrdinal, long bytes);
        void onPresentedFrames(long count);
        long syncWaitStart();
        void syncWaitEnd(long startedNs);
        void onBatchSubmitted(Object renderer, long sequence, long javaPrepareNs, long nativeCallNs);
    }

    private RayniumHarnessBridge() {
    }

    public static boolean isRayniumLoaded() {
        return FabricLoader.getInstance().isModLoaded("raynium");
    }

    public static boolean isTerrainReady() {
        return !isRayniumLoaded() || RayniumCoreBridge.isTerrainReady();
    }

    public static String activeBackendName() {
        return isRayniumLoaded() ? RayniumCoreBridge.activeBackendName() : "vulkan";
    }

    public static Object activeNativeRenderer() {
        return isRayniumLoaded() ? RayniumCoreBridge.activeNativeRenderer() : null;
    }

    public static void installBenchmarkTelemetry(BenchmarkCallbacks callbacks) {
        if (isRayniumLoaded()) {
            RayniumCoreBridge.installBenchmarkTelemetry(callbacks);
        }
    }

    public static void installValidationTelemetry(Consumer<Object> onPipelineSelected) {
        if (isRayniumLoaded()) {
            RayniumCoreBridge.installValidationTelemetry(onPipelineSelected);
        }
    }

    public static void drainTelemetry(Object renderer, Consumer<MemorySegment> consumer) {
        if (renderer != null) {
            RayniumCoreBridge.drainTelemetry(renderer, consumer);
        }
    }

    public static void flushAndDrain(Object renderer, Consumer<MemorySegment> consumer) {
        if (renderer != null) {
            RayniumCoreBridge.flushAndDrain(renderer, consumer);
        }
    }
}
