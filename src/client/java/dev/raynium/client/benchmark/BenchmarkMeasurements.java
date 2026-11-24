package dev.raynium.client.benchmark;

import com.google.gson.JsonObject;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Fixed storage for opt-in sampling; sorting and file output happen after the measured route. */
final class BenchmarkMeasurements {
    static final String[] COLUMNS={"elapsedNs","clientFrameIntervalNs","renderWallNs","coarseRenderThreadCpuNs",
        "javaPrepareNs","nativeCallNs","nativeWaitNs","nativePrepareNs","nativeRdgRunNs","nativePresentNs",
        "terrainUploadBytes","textureUploadBytes","sceneUploadBytes","guiUploadBytes",
        "terrainDrawRecords","terrainDrawCalls","sceneDrawCalls","guiDrawCalls","geometryAndPostBindCalls",
        "terrainVertexCapacityBytes","terrainIndexCapacityBytes","terrainAllocatedBytes","nativeSyncWaitNs"};
    private final long[][] frames;
    private final long[] gpu;
    private int frameCount,gpuCount;
    BenchmarkMeasurements(int capacity) { frames=new long[COLUMNS.length][capacity]; gpu=new long[capacity]; }
    void frame(long elapsed,long interval,long wall,long cpu,long[] nativeTimes) {
        if(frameCount==gpu.length) throw new IllegalStateException("Benchmark frame storage exhausted");
        frames[0][frameCount]=elapsed; frames[1][frameCount]=interval; frames[2][frameCount]=wall; frames[3][frameCount]=cpu;
        for(int i=0;i<nativeTimes.length;i++) frames[4+i][frameCount]=nativeTimes[i];
        ++frameCount;
    }
    void gpu(long nanos) {
        if(nanos<0 || gpuCount==gpu.length) throw new IllegalStateException("Invalid or excessive GPU samples");
        gpu[gpuCount++]=nanos;
    }
    static JsonObject distribution(long[] values,int count) {
        return distribution(values,count,true);
    }
    static JsonObject distribution(long[] values,int count,boolean nanos) {
        var result=new JsonObject(); long[] valid=new long[count]; int size=0;
        for(int i=0;i<count;i++) if(values[i]>=0) valid[size++]=values[i];
        result.addProperty("samples",size); if(size==0) return result;
        Arrays.sort(valid,0,size);
        double scale=nanos?1e6:1; String suffix=nanos?"Ms":"";
        result.addProperty("p50"+suffix,valid[(int)Math.ceil(size*.5)-1]/scale);
        result.addProperty("p95"+suffix,valid[(int)Math.ceil(size*.95)-1]/scale);
        result.addProperty("p99"+suffix,valid[(int)Math.ceil(size*.99)-1]/scale);
        result.addProperty("max"+suffix,valid[size-1]/scale);
        if(!nanos) { long total=0; for(int i=0;i<size;i++) total=Math.addExact(total,valid[i]); result.addProperty("sum",total); }
        return result;
    }
    JsonObject finish(Path directory) throws IOException {
        var result=new JsonObject(); for(int i=1;i<COLUMNS.length;i++) if(i!=3) result.add(COLUMNS[i],distribution(frames[i],frameCount,COLUMNS[i].endsWith("Ns")));
        long cpu=0; for(int i=0;i<frameCount;i++) cpu+=Math.max(0,frames[3][i]);
        result.addProperty("coarseRenderThreadCpuTotalNs",cpu);
        result.add("gpuFrameNs",distribution(gpu,gpuCount));
        try(var writer=Files.newBufferedWriter(directory.resolve("frames.csv"))) {
            writer.write(String.join(",",COLUMNS)); writer.newLine();
            for(int row=0;row<frameCount;row++) {
                for(int column=0;column<COLUMNS.length;column++) { if(column!=0) writer.write(','); writer.write(Long.toString(frames[column][row])); }
                writer.newLine();
            }
        }
        try(var writer=Files.newBufferedWriter(directory.resolve("gpu.csv"))) {
            writer.write("gpuFrameNs\n"); for(int i=0;i<gpuCount;i++) { writer.write(Long.toString(gpu[i])); writer.newLine(); }
        }
        return result;
    }
}
