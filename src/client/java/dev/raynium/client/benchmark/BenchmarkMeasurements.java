package dev.raynium.client.benchmark;

import com.google.gson.JsonObject;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Chunked storage for opt-in sampling; sorting and file output happen after the measured route. */
final class BenchmarkMeasurements {
    static final String[] COLUMNS={"elapsedNs","clientFrameIntervalNs","renderWallNs","coarseRenderThreadCpuNs",
        "javaPrepareNs","nativeCallNs","nativeWaitNs","nativePrepareNs","nativeRdgRunNs","nativePresentNs",
        "terrainUploadBytes","textureUploadBytes","sceneUploadBytes","guiUploadBytes",
        "terrainDrawRecords","terrainDrawCalls","sceneDrawCalls","guiDrawCalls","geometryAndPostBindCalls",
        "terrainVertexCapacityBytes","terrainIndexCapacityBytes","terrainAllocatedBytes","nativeSyncWaitNs"};
    private static final class Column {
        private static final int CHUNK=4096;
        private final int capacity;
        private final long[][] chunks;

        Column(int capacity) {
            this.capacity=capacity;
            chunks=new long[Math.ceilDiv(capacity,CHUNK)][];
        }
        long get(int row) {
            var chunk=chunks[row/CHUNK];
            return chunk==null?0:chunk[row%CHUNK];
        }
        void set(int row,long value) {
            int index=row/CHUNK;
            if(chunks[index]==null) chunks[index]=new long[Math.min(CHUNK,capacity-index*CHUNK)];
            chunks[index][row%CHUNK]=value;
        }
    }

    private final Column[] frames=new Column[COLUMNS.length];
    private final Column gpu;
    private static final class FrameSamples {
        final int row;
        int expected,received;
        boolean missingGpu;
        long gpu;
        FrameSamples(int row) { this.row=row; }
    }
    private FrameSamples currentSamples;
    private final it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<FrameSamples> batchRows=new it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<>();

    private int frameCount,gpuCount;
    BenchmarkMeasurements(int capacity) {
        Arrays.setAll(frames,i->new Column(capacity));
        gpu=new Column(capacity);
    }
    void frame(long elapsed,long interval,long wall,long cpu,long[] nativeTimes) {
        if(frameCount==gpu.capacity) throw new IllegalStateException("Benchmark frame storage exhausted");
        frames[0].set(frameCount,elapsed); frames[1].set(frameCount,interval); frames[2].set(frameCount,wall); frames[3].set(frameCount,cpu);
        for(int i=0;i<nativeTimes.length;i++) {
            if(currentSamples==null || currentSamples.received==0 || i<2 || i>17) frames[4+i].set(frameCount,nativeTimes[i]);
        }
        ++frameCount;
        if(currentSamples!=null) complete(currentSamples);
        currentSamples=null;
    }
    void expect(long sequence) {
        if(currentSamples==null) currentSamples=new FrameSamples(frameCount);
        currentSamples.expected++;
        batchRows.put(sequence,currentSamples);
    }
    void receive(java.lang.foreign.MemorySegment record) {
        var row=batchRows.remove(record.get(java.lang.foreign.ValueLayout.JAVA_LONG,0));
        if(row==null) return;
        batch(row.row,record);
        if((record.get(java.lang.foreign.ValueLayout.JAVA_INT,8)&1)==0) row.missingGpu=true;
        else row.gpu+=record.get(java.lang.foreign.ValueLayout.JAVA_LONG,16);
        ++row.received;
        complete(row);
    }
    private void complete(FrameSamples row) {
        if(row.row<frameCount && row.received==row.expected && !row.missingGpu) gpu(row.gpu);
    }
    void batch(int row,java.lang.foreign.MemorySegment record) {
        for(int column=6;column<=21;++column) {
            long value=record.get(java.lang.foreign.ValueLayout.JAVA_LONG,24L+(column-6)*8L);
            frames[column].set(row,column<=18?Math.max(0,frames[column].get(row))+value:value);
        }
    }
    void gpu(long nanos) {
        if(nanos<0 || gpuCount==gpu.capacity) throw new IllegalStateException("Invalid or excessive GPU samples");
        gpu.set(gpuCount++,nanos);
    }
    static JsonObject distribution(long[] values,int count) {
        return distribution(values,count,true);
    }
    static JsonObject distribution(long[] values,int count,boolean nanos) {
        return distribution(i->values[i],count,nanos);
    }
    private static JsonObject distribution(java.util.function.IntToLongFunction values,int count,boolean nanos) {
        var result=new JsonObject(); long[] valid=new long[count]; int size=0;
        for(int i=0;i<count;i++) { long value=values.applyAsLong(i); if(value>=0) valid[size++]=value; }
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
        var result=new JsonObject(); for(int i=1;i<COLUMNS.length;i++) if(i!=3) result.add(COLUMNS[i],distribution(frames[i]::get,frameCount,COLUMNS[i].endsWith("Ns")));
        long cpu=0; for(int i=0;i<frameCount;i++) cpu+=Math.max(0,frames[3].get(i));
        result.addProperty("coarseRenderThreadCpuTotalNs",cpu);
        result.add("gpuFrameNs",distribution(gpu::get,gpuCount,true));
        try(var writer=Files.newBufferedWriter(directory.resolve("frames.csv"))) {
            writer.write(String.join(",",COLUMNS)); writer.newLine();
            for(int row=0;row<frameCount;row++) {
                for(int column=0;column<COLUMNS.length;column++) { if(column!=0) writer.write(','); writer.write(Long.toString(frames[column].get(row))); }
                writer.newLine();
            }
        }
        try(var writer=Files.newBufferedWriter(directory.resolve("gpu.csv"))) {
            writer.write("gpuFrameNs\n"); for(int i=0;i<gpuCount;i++) { writer.write(Long.toString(gpu.get(i))); writer.newLine(); }
        }
        return result;
    }
}
