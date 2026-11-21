package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.commands.GpuQueryPool;
import dev.raynium.bridge.NativeRenderer;
import java.lang.foreign.Arena;
import java.util.Arrays;
import java.util.Objects;
import java.util.OptionalLong;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public final class RayniumQueryPool implements GpuQueryPool {
    private final NativeRenderer renderer;
    private final BatchSubmission[] latest;
    private final int id;
    private int recordingReferences;
    long lastRetainedTapeId;
    private boolean closed;

    public RayniumQueryPool(NativeRenderer renderer,int count) {
        if(count<=0) throw new IllegalArgumentException("Query pool must have a positive size");
        this.renderer=renderer;
        latest=new BatchSubmission[count];
        id=renderer.createQueryPool(count);
    }
    int id() { return id; }
    @Override public int size() { return latest.length; }
    void validateWrite(NativeRenderer owner,int index) {
        if(closed || owner!=renderer || index<0 || index>=size()) throw new IllegalStateException("Invalid timestamp query slot");
    }
    void retainRecording() { ++recordingReferences; }
    void releaseRecording() {
        if(--recordingReferences==0 && closed) renderer.destroyQueryPool(id);
    }
    void written(int index,BatchSubmission batch) { latest[index]=batch; }
    @Override public OptionalLong getValue(int index) { return getValues(index,1)[0]; }
    @Override public OptionalLong[] getValues(int index,int count) {
        if(closed) throw new IllegalStateException("Query pool is closed");
        Objects.checkFromIndexSize(index,count,size());
        var result=new OptionalLong[count];
        Arrays.fill(result,OptionalLong.empty());
        boolean anySubmitted=false;
        for(int i=0;i<count;++i) {
            var batch=latest[index+i];
            if(batch!=null && batch.submittedSequence()>0) { anySubmitted=true; break; }
        }
        if(anySubmitted) try(var arena=Arena.ofConfined()) {
            var samples=arena.allocate(16L*count,8);
            renderer.syncQueryPool(id,index,count,samples);
            for(int i=0;i<count;++i) {
                var batch=latest[index+i];
                long sequence=samples.get(JAVA_LONG,16L*i);
                if(sequence>0 && batch!=null && batch.submittedSequence()==sequence)
                    result[i]=OptionalLong.of(samples.get(JAVA_LONG,16L*i+8));
            }
        }
        return result;
    }
    @Override public void close() {
        if(closed) return;
        closed=true;
        if(recordingReferences==0) renderer.destroyQueryPool(id);
    }
}
