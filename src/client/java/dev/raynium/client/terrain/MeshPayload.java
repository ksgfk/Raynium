package dev.raynium.client.terrain;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;

/** The byte array is owned by this payload and never aliases a compiler arena. */
public final class MeshPayload {
    private static final AtomicLong IDS=new AtomicLong();
    public final long id=IDS.incrementAndGet();
    public final long epoch;
    public final byte[] vertices;
    public final int[] groupVertices=new int[7];
    private final float[] minimum={Float.POSITIVE_INFINITY,Float.POSITIVE_INFINITY,Float.POSITIVE_INFINITY};
    private final float[] maximum={Float.NEGATIVE_INFINITY,Float.NEGATIVE_INFINITY,Float.NEGATIVE_INFINITY};
    public MeshPayload(long epoch, ByteBuffer source) {
        this.epoch=epoch;
        byte[] original=new byte[source.remaining()]; source.duplicate().get(original);
        if(original.length%112!=0) throw new IllegalArgumentException("SOLID mesh is not a 28-byte quad stream");
        var data=ByteBuffer.wrap(original).order(ByteOrder.nativeOrder());
        int[] groups=new int[original.length/112];
        for(int q=0;q<groups.length;q++) {
            for(int v=0;v<4;v++) for(int axis=0;axis<3;axis++) {
                float p=data.getFloat(q*112+v*28+axis*4);
                minimum[axis]=Math.min(minimum[axis],p); maximum[axis]=Math.max(maximum[axis],p);
            }
            groups[q]=group(data,q*112); groupVertices[groups[q]]+=4;
        }
        int[] offsets=new int[7];
        for(int i=1;i<7;i++) offsets[i]=offsets[i-1]+groupVertices[i-1]*28;
        vertices=new byte[original.length];
        for(int q=0;q<groups.length;q++) { int g=groups[q]; System.arraycopy(original,q*112,vertices,offsets[g],112); offsets[g]+=112; }
    }
    // Only exactly planar, consistently wound axis-aligned quads enter a cullable group.
    private static int group(ByteBuffer data,int offset) {
        for(int v=0;v<4;v++) for(int axis=0;axis<3;axis++) if(!Float.isFinite(data.getFloat(offset+v*28+axis*4))) return 6;
        for(int a=0;a<3;a++) {
            float plane=data.getFloat(offset+a*4);
            if(plane!=data.getFloat(offset+28+a*4) || plane!=data.getFloat(offset+56+a*4) || plane!=data.getFloat(offset+84+a*4)) continue;
            int b=(a+1)%3,c=(a+2)%3;
            double b0=data.getFloat(offset+b*4),c0=data.getFloat(offset+c*4);
            double b1=data.getFloat(offset+28+b*4)-b0,c1=data.getFloat(offset+28+c*4)-c0;
            double b2=data.getFloat(offset+56+b*4)-b0,c2=data.getFloat(offset+56+c*4)-c0;
            double b3=data.getFloat(offset+84+b*4)-b0,c3=data.getFloat(offset+84+c*4)-c0;
            double n=b1*c2-c1*b2,m=b2*c3-c2*b3;
            if(n>0 && m>0) return a*2;
            if(n<0 && m<0) return a*2+1;
        }
        return 6;
    }
    public int visibleGroups(double x,double y,double z) {
        double[] eye={x,y,z}; int mask=127;
        for(int a=0;a<3;a++) {
            if(!Double.isFinite(eye[a]) || !Float.isFinite(minimum[a]) || !Float.isFinite(maximum[a])) return 127;
            if(eye[a]<minimum[a]-0.001) mask &= ~(1<<(a*2));
            if(eye[a]>maximum[a]+0.001) mask &= ~(1<<(a*2+1));
        }
        return mask;
    }
}
