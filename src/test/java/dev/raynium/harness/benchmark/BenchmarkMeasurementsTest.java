package dev.raynium.harness.benchmark;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.foreign.Arena;
import java.lang.foreign.ValueLayout;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
class BenchmarkMeasurementsTest {
    @Test
    void reportsAcrossChunkBoundaries(@TempDir Path directory) throws Exception {
        var m = new BenchmarkMeasurements(4097);
        var timings = new long[19];
        for (int i = 0; i < 4097; ++i) {
            m.frame(i, i, 0, 0, timings);
            m.gpu(i);
        }
        assertThrows(IllegalStateException.class, () -> m.gpu(0));
        assertThrows(IllegalStateException.class, () -> m.frame(0, 0, 0, 0, timings));
        var stats = m.finish(directory);
        assertEquals(4097, stats.getAsJsonObject("gpuFrameNs").get("samples").getAsInt());
        assertTrue(Files.readAllLines(directory.resolve("frames.csv")).getLast().startsWith("4096,4096,"));
    }

    @Test
    void externalWaitHasItsOwnNanosecondColumn(@TempDir Path directory) throws Exception {
        var measurements = new BenchmarkMeasurements(1);
        long[] nativeTimes = new long[19];
        nativeTimes[1] = 400_000;
        nativeTimes[18] = 2_000_000;
        measurements.frame(0, 1, 3_000_000, 0, nativeTimes);
        var stats = measurements.finish(directory);
        assertEquals(2.0, stats.getAsJsonObject("nativeSyncWaitNs").get("p50Ms").getAsDouble());
        assertEquals(0.4, stats.getAsJsonObject("nativeCallNs").get("p50Ms").getAsDouble());
        var lines = Files.readAllLines(directory.resolve("frames.csv"));
        assertTrue(lines.get(0).endsWith(",nativeSyncWaitNs"));
        assertTrue(lines.get(1).endsWith(",2000000"));
    }
    @Test
    void lateBatchesFillTheOwningFrameAndExcludePartiallyMeasuredGpuTime(@TempDir Path directory) throws Exception {
        var measurements = new BenchmarkMeasurements(3);
        long[] times = new long[19];
        Arrays.fill(times, -1);
        try (var arena = Arena.ofConfined()) {
            var records = arena.allocate(3L * 152, 8);
            for (int i = 0; i < 3; ++i) {
                var record = records.asSlice(152L * i, 152);
                record.set(ValueLayout.JAVA_LONG, 0, i + 1L)
                ;
                record.set(ValueLayout.JAVA_INT, 8, i == 1 ? 0 : 1)
                ;
                record.set(ValueLayout.JAVA_LONG, 16, i == 1 ? 0 : 3_000_000L + i)
                ;
                record.set(ValueLayout.JAVA_LONG, 24, (i + 1) * 100L)
                ;
            }
            measurements.expect(1);
            measurements.expect(2);
            measurements.frame(0, 1, 2, 3, times);
            measurements.expect(3);
            measurements.receive(records.asSlice(0, 152));
            measurements.receive(records.asSlice(152, 152));
            measurements.receive(records.asSlice(304, 152));
            measurements.frame(1, 1, 2, 3, times);
        }
        var stats = measurements.finish(directory);
        assertEquals(1, stats.getAsJsonObject("gpuFrameNs").get("samples").getAsInt());
        assertEquals(3.000002, stats.getAsJsonObject("gpuFrameNs").get("p50Ms").getAsDouble());
        var lines = Files.readAllLines(directory.resolve("frames.csv"));
        assertEquals("300", lines.get(1).split(",")[6]);
        assertEquals("300", lines.get(2).split(",")[6]);
    }

    @Test
    void byteAndCallCountsRemainUnscaledAndExcludeUnavailableReferenceValues() {
        var stats = BenchmarkMeasurements.distribution(new long[] {-1, 256, 512, 1024}, 4, false);
        assertEquals(512, stats.get("p50").getAsLong());
        assertEquals(1792, stats.get("sum").getAsLong());
        assertFalse(stats.has("p50Ms"));
    }
    @Test
    void quantilesUseOnlyAvailableSamplesAndRetainTailSpikes() {
        var stats = BenchmarkMeasurements.distribution(new long[] {-1, 1_000_000, 2_000_000, 3_000_000, 100_000_000}, 5);
        assertEquals(4, stats.get("samples").getAsInt());
        assertEquals(2, stats.get("p50Ms").getAsDouble());
        assertEquals(100, stats.get("p99Ms").getAsDouble());
        assertFalse(BenchmarkMeasurements.distribution(new long[] {-1}, 1).has("p50Ms"));
    }
}
