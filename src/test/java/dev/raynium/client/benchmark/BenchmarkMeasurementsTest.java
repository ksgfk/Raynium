package dev.raynium.client.benchmark;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class BenchmarkMeasurementsTest {
    @Test void byteAndCallCountsRemainUnscaledAndExcludeUnavailableReferenceValues() {
        var stats=BenchmarkMeasurements.distribution(new long[]{-1,256,512,1024},4,false);
        assertEquals(512,stats.get("p50").getAsLong()); assertEquals(1792,stats.get("sum").getAsLong());
        assertFalse(stats.has("p50Ms"));
    }
    @Test void quantilesUseOnlyAvailableSamplesAndRetainTailSpikes() {
        var stats=BenchmarkMeasurements.distribution(new long[]{-1,1_000_000,2_000_000,3_000_000,100_000_000},5);
        assertEquals(4,stats.get("samples").getAsInt()); assertEquals(2,stats.get("p50Ms").getAsDouble());
        assertEquals(100,stats.get("p99Ms").getAsDouble());
        assertFalse(BenchmarkMeasurements.distribution(new long[]{-1},1).has("p50Ms"));
    }
}
