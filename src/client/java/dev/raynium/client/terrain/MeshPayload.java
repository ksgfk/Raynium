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
    public final int allElementMask;
    private final long groupElementBits;
    private final float minX,minY,minZ,maxX,maxY,maxZ;
    private final boolean finiteBounds;
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
        float lx=Float.POSITIVE_INFINITY,ly=lx,lz=lx,hx=Float.NEGATIVE_INFINITY,hy=hx,hz=hx;
        int[] groups=new int[data.remaining()/112];
        for(int q=0;q<groups.length;q++) {
            for(int v=0;v<4;v++) {
                int o=q*112+v*28;
                float x=data.getFloat(o),y=data.getFloat(o+4),z=data.getFloat(o+8);
                lx=Math.min(lx,x); ly=Math.min(ly,y); lz=Math.min(lz,z);
                hx=Math.max(hx,x); hy=Math.max(hy,y); hz=Math.max(hz,z);
            }
            groups[q]=mode==2?6:group(data,q*112); groupVertices[groups[q]]+=4;
        }
        int[] offsets=new int[7];
        for(int i=1;i<7;i++) offsets[i]=offsets[i-1]+groupVertices[i-1]*28;
        vertices=new byte[data.remaining()];
        for(int q=0;q<groups.length;q++) { int g=groups[q]; data.get(q*112,vertices,offsets[g],112); offsets[g]+=112; }
        minX=lx; minY=ly; minZ=lz; maxX=hx; maxY=hy; maxZ=hz;
        finiteBounds=Float.isFinite(lx)&&Float.isFinite(ly)&&Float.isFinite(lz)&&Float.isFinite(hx)&&Float.isFinite(hy)&&Float.isFinite(hz);
        long bits=0; int elements=0;
        for(int g=0;g<7;g++) if(groupVertices[g]>0) bits|=(1L<<elements++)<<(g*8);
        groupElementBits=bits; allElementMask=(1<<elements)-1;
    }
    public IndexSnapshot copyIndices(ByteBuffer source,int stride) {
        var data=source.slice().order(ByteOrder.nativeOrder());
        int count=vertices.length/112*6;
        if((stride!=2 && stride!=4) || data.remaining()!=count*stride) throw new IllegalArgumentException("Invalid transparent indices");
        int[] values=new int[count];
        for(int i=0;i<count;i++) {
            int value=stride==2?Short.toUnsignedInt(data.getShort()):data.getInt();
            if(value<0 || value>=vertices.length/28) throw new IllegalArgumentException("Transparent index outside mesh");
            values[i]=value;
        }
        return new IndexSnapshot(values);
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
    public int visibleElementMask(double x,double y,double z) {
        if(!finiteBounds || !Double.isFinite(x) || !Double.isFinite(y) || !Double.isFinite(z)) return allElementMask;
        int mask=element(6);
        if(x>=minX-0.001) mask|=element(0);
        if(x<=maxX+0.001) mask|=element(1);
        if(y>=minY-0.001) mask|=element(2);
        if(y<=maxY+0.001) mask|=element(3);
        if(z>=minZ-0.001) mask|=element(4);
        if(z<=maxZ+0.001) mask|=element(5);
        return mask;
    }
    private int element(int group) { return (int)(groupElementBits>>>(group*8))&0x7f; }
}
