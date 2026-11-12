package dev.raynium.client.terrain;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;

/** The byte array is owned by this payload and never aliases a compiler arena. */
public final class MeshPayload {
    private static final AtomicLong IDS=new AtomicLong();
    public final long id=IDS.incrementAndGet();
    public final long epoch;
    public byte[] vertices;
    public final int vertexCount;
    public final int mode;
    public volatile IndexSnapshot indices;
    public record IndexSnapshot(int[] values) {}
    public MeshPayload(long epoch, ByteBuffer source) {
        this(epoch,source,0);
    }
    public MeshPayload(long epoch,ByteBuffer source,int mode) {
        this.epoch=epoch; this.mode=mode;
        var data=source.slice().order(ByteOrder.nativeOrder());
        if(data.remaining()%112!=0) throw new IllegalArgumentException("Terrain mesh is not a 28-byte quad stream");
        vertexCount=data.remaining()/28;
        vertices=new byte[data.remaining()]; data.get(vertices);
    }
    public IndexSnapshot copyIndices(ByteBuffer source,int stride) {
        var data=source.slice().order(ByteOrder.nativeOrder());
        int count=vertexCount/4*6;
        if((stride!=2 && stride!=4) || data.remaining()!=count*stride) throw new IllegalArgumentException("Invalid transparent indices");
        int[] values=new int[count];
        for(int i=0;i<count;i++) {
            int value=stride==2?Short.toUnsignedInt(data.getShort()):data.getInt();
            if(value<0 || value>=vertexCount) throw new IllegalArgumentException("Transparent index outside mesh");
            values[i]=value;
        }
        return new IndexSnapshot(values);
    }
}
