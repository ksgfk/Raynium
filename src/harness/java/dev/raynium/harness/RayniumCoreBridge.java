package dev.raynium.harness;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.client.config.RayniumConfig;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.terrain.TerrainCapture;
import dev.raynium.diagnostics.RayniumTelemetry;
import java.io.IOException;
import java.lang.foreign.MemorySegment;
import java.util.function.Consumer;
import net.minecraft.client.Minecraft;

/** Loaded exclusively when the Raynium core mod is present on the classpath. */
final class RayniumCoreBridge {
    private RayniumCoreBridge() {
    }

    static boolean isTerrainReady() {
        return TerrainCapture.active();
    }

    static String activeBackendName() {
        var device = RenderSystem.getDevice();
        if (device instanceof RayniumGpuDevice raynium) {
            return raynium.backend().propertyValue();
        }
        try {
            return RayniumConfig.load(Minecraft.getInstance().gameDirectory.toPath()).backend().propertyValue();
        } catch (IOException | RuntimeException ignored) {
            return RayniumConfig.DEFAULT.backend().propertyValue();
        }
    }

    static Object activeNativeRenderer() {
        var device = RenderSystem.getDevice();
        return device instanceof RayniumGpuDevice raynium ? raynium.nativeRenderer() : null;
    }

    static void installBenchmarkTelemetry(RayniumHarnessBridge.BenchmarkCallbacks callbacks) {
        RayniumTelemetry.setListener(new RayniumTelemetry.TelemetryListener() {
            @Override
            public boolean isMeasuring() {
                return callbacks.isMeasuring();
            }

            @Override
            public void onMappedWrites(int writeSourceOrdinal, long bytes) {
                callbacks.onMappedWrites(writeSourceOrdinal, bytes);
            }

            @Override
            public void onPresentedFrames(long count) {
                callbacks.onPresentedFrames(count);
            }

            @Override
            public long syncWaitStart() {
                return callbacks.syncWaitStart();
            }

            @Override
            public void syncWaitEnd(long startedNs) {
                callbacks.syncWaitEnd(startedNs);
            }

            @Override
            public void onBatchSubmitted(Object renderer, long sequence, long javaPrepareNs, long nativeCallNs) {
                callbacks.onBatchSubmitted(renderer, sequence, javaPrepareNs, nativeCallNs);
            }
        });
    }

    static void installValidationTelemetry(Consumer<Object> onPipelineSelected) {
        RayniumTelemetry.setListener(new RayniumTelemetry.TelemetryListener() {
            @Override
            public void onPipelineSelected(Object renderPipeline) {
                onPipelineSelected.accept(renderPipeline);
            }
        });
    }

    static void drainTelemetry(Object rendererObject, Consumer<MemorySegment> consumer) {
        ((NativeRenderer)rendererObject).drainTelemetry(consumer);
    }

    static void flushAndDrain(Object rendererObject, Consumer<MemorySegment> consumer) {
        var renderer = (NativeRenderer)rendererObject;
        if (renderer.isClosed()) {
            return;
        }
        Throwable failure = null;
        try {
            if (renderer.isPoisoned()) {
                throw new IllegalStateException("Renderer is terminal");
            }
            renderer.flush();
        } catch (Throwable flushFailure) {
            failure = flushFailure;
        }
        try {
            renderer.drainTelemetry(consumer);
        } catch (Throwable drainFailure) {
            if (failure == null) {
                failure = drainFailure;
            } else if (failure != drainFailure) {
                failure.addSuppressed(drainFailure);
            }
        }
        if (failure instanceof Error error) {
            throw error;
        }
        if (failure instanceof RuntimeException runtime) {
            throw runtime;
        }
        if (failure != null) {
            throw new IllegalStateException("Benchmark renderer flush failed", failure);
        }
    }
}
