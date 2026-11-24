package dev.raynium.client.benchmark;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class BenchmarkMeasurementsTest {
    @Test void externalWaitHasItsOwnNanosecondColumn(@org.junit.jupiter.api.io.TempDir java.nio.file.Path directory) throws Exception {
        var measurements=new BenchmarkMeasurements(1);
        long[] nativeTimes=new long[19];
        nativeTimes[1]=400_000;
        nativeTimes[18]=2_000_000;
        measurements.frame(0,1,3_000_000,0,nativeTimes);
        var stats=measurements.finish(directory);
        assertEquals(2.0,stats.getAsJsonObject("nativeSyncWaitNs").get("p50Ms").getAsDouble());
        assertEquals(0.4,stats.getAsJsonObject("nativeCallNs").get("p50Ms").getAsDouble());
        var lines=java.nio.file.Files.readAllLines(directory.resolve("frames.csv"));
        assertTrue(lines.get(0).endsWith(",nativeSyncWaitNs"));
        assertTrue(lines.get(1).endsWith(",2000000"));
    }
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
