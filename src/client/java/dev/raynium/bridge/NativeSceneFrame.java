package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import static java.lang.foreign.ValueLayout.*;

/** Reused scene tables; borrowed geometry must remain alive until presentPrepared returns. */
public final class NativeSceneFrame implements AutoCloseable {
    public static final int SKY=0,SOLID=1,TRANSLUCENT=2,AFTER_TERRAIN=3,SEE_THROUGH=4,ALWAYS_ON_TOP=5,HAND=6,PANORAMA=7;
    public static final int OIT_BOUNDS=8,OIT_TRANSMITTANCE=9,OIT_ACCUMULATE=10;
    public static final int WATER_MASK=11;
    public static final int CLOUD_BOUNDS=12,CLOUD_TRANSMITTANCE=13,CLOUD_ACCUMULATE=14;
    public static final int OFFSCREEN=15,OUTLINE=16,GUI=17;
    private final NativeFrameBuffer.Storage bytes=new NativeFrameBuffer.Storage(),blobs=new NativeFrameBuffer.Storage(),
        bindings=new NativeFrameBuffer.Storage(),draws=new NativeFrameBuffer.Storage(),targets=new NativeFrameBuffer.Storage(),effects=new NativeFrameBuffer.Storage();
    private final ArrayList<MemorySegment> borrowed=new ArrayList<>();
    private long cursor;
    private int blobCount,bindingCount,drawCount,targetCount,effectCount,guiBlurSplit,activeTarget=-1;
    void begin() { cursor=0; blobCount=bindingCount=drawCount=targetCount=effectCount=guiBlurSplit=0; activeTarget=-1; borrowed.clear(); }
    public void effect(int chain,int phase,int globalsBlob) {
        if(chain<1 || chain>5 || chain==3 || phase<0 || phase>1 || (phase==1 && chain!=1) || globalsBlob<0 || globalsBlob>=blobCount)
            throw new IllegalArgumentException("Invalid built-in post effect");
        effects.reserve((effectCount+1L)*16,true); long at=effectCount++*16L;
        effects.segment.set(JAVA_INT,at,chain); effects.segment.set(JAVA_INT,at+4,phase);
        effects.segment.set(JAVA_INT,at+8,globalsBlob); effects.segment.set(JAVA_INT,at+12,0);
    }
    public void guiBlurSplit(int split) { if(split<0) throw new IllegalArgumentException("Negative GUI split"); guiBlurSplit=split; }
    public void beginTarget(int texture,int x,int y,int width,int height) {
        if(activeTarget!=-1 || x<0 || y<0 || width<=0 || height<=0) throw new IllegalArgumentException("Invalid or nested scene target");
        targets.reserve((targetCount+1L)*32,true); activeTarget=targetCount++; long at=activeTarget*32L;
        targets.segment.set(JAVA_INT,at,texture); targets.segment.set(JAVA_INT,at+4,x); targets.segment.set(JAVA_INT,at+8,y);
        targets.segment.set(JAVA_INT,at+12,width); targets.segment.set(JAVA_INT,at+16,height);
        targets.segment.set(JAVA_INT,at+20,drawCount); targets.segment.set(JAVA_INT,at+24,0); targets.segment.set(JAVA_INT,at+28,0);
    }
    public void endTarget() {
        if(activeTarget<0) throw new IllegalStateException("No active scene target");
        long at=activeTarget*32L; targets.segment.set(JAVA_INT,at+24,drawCount-targets.segment.get(JAVA_INT,at+20)); activeTarget=-1;
    }
    public int copy(MemorySegment source) {
        long offset=(cursor+255)&~255L;
        cursor=Math.addExact(offset,source.byteSize()); bytes.reserve(cursor,true);
        MemorySegment.copy(source,0,bytes.segment,offset,source.byteSize());
        return blob(null,offset,source.byteSize());
    }
    public int borrow(MemorySegment source) {
        if(!source.isNative()) throw new IllegalArgumentException("Scene geometry must be off heap");
        return blob(source,0,source.byteSize());
    }
    private int blob(MemorySegment source,long offset,long size) {
        blobs.reserve((blobCount+1L)*16,true);
        borrowed.add(source); int index=blobCount++;
        // Until finish, this field stores an offset into the growable byte storage.
        blobs.segment.set(JAVA_LONG,index*16L,offset); blobs.segment.set(JAVA_LONG,index*16L+8,size); return index;
    }
    public int bindingCount() { return bindingCount; }
    public void constant(int binding,int blob,long offset,long size) { binding(0,binding,blob,0,offset,size); }
    public void texture(int binding,int texture,int samplerBits) { texture(binding,texture,samplerBits,0,0,1000,1); }
    public void texture(int binding,int texture,int samplerBits,int baseMip,int mipCount,float maxLod,int anisotropy) {
        if(baseMip<0 || mipCount<0 || !Float.isFinite(maxLod) || maxLod<0 || anisotropy<1) throw new IllegalArgumentException("Invalid texture sampling");
        binding(1,binding,texture,samplerBits,0,0); long at=(bindingCount-1L)*48+32;
        bindings.segment.set(JAVA_INT,at,baseMip); bindings.segment.set(JAVA_INT,at+4,mipCount);
        bindings.segment.set(JAVA_FLOAT,at+8,maxLod); bindings.segment.set(JAVA_INT,at+12,anisotropy);
    }
    public void graphTexture(int binding,int resource) { binding(2,binding,resource,0,0,0); }
    public void signedByteBuffer(int binding,int resource,long offset,long size) { binding(3,binding,resource,0,offset,size); }
    private void binding(int kind,int binding,int resource,int sampler,long offset,long size) {
        bindings.reserve((bindingCount+1L)*48,true); long at=bindingCount++*48L; var s=bindings.segment;
        s.set(JAVA_INT,at,kind); s.set(JAVA_INT,at+4,binding); s.set(JAVA_INT,at+8,resource); s.set(JAVA_INT,at+12,sampler);
        s.set(JAVA_LONG,at+16,offset); s.set(JAVA_LONG,at+24,size);
        s.set(JAVA_LONG,at+32,0); s.set(JAVA_LONG,at+40,0);
    }
    public int draw(int pipeline,int phase,int vertices,int indices,int vertexCount,int indexCount,int indexStride,
            int firstBinding,int instanceCount,int instances,long vertexOffset,long indexOffset,long instanceOffset,
            int x,int y,int width,int height) {
        if((activeTarget>=0)!=(phase==OFFSCREEN)) throw new IllegalArgumentException("Scene draw target and phase disagree");
        draws.reserve((drawCount+1L)*88,true); long at=drawCount++*88L; var s=draws.segment;
        s.set(JAVA_INT,at,pipeline); s.set(JAVA_INT,at+4,phase); s.set(JAVA_INT,at+8,vertices); s.set(JAVA_INT,at+12,indices);
        s.set(JAVA_INT,at+16,vertexCount); s.set(JAVA_INT,at+20,indexCount); s.set(JAVA_INT,at+24,indexStride);
        s.set(JAVA_INT,at+28,firstBinding); s.set(JAVA_INT,at+32,bindingCount-firstBinding); s.set(JAVA_INT,at+36,instanceCount);
        s.set(JAVA_INT,at+40,instances); s.set(JAVA_INT,at+44,0);
        s.set(JAVA_LONG,at+48,vertexOffset); s.set(JAVA_LONG,at+56,indexOffset); s.set(JAVA_LONG,at+64,instanceOffset);
        s.set(JAVA_INT,at+72,x); s.set(JAVA_INT,at+76,y); s.set(JAVA_INT,at+80,width); s.set(JAVA_INT,at+84,height);
        return drawCount-1;
    }
    void finish(MemorySegment descriptor) {
        if(activeTarget>=0) throw new IllegalStateException("Scene target is still open");
        // Use a separate pointer table so finish remains idempotent across nested GUI frames.
        descriptor.set(ADDRESS,328,blobCount==0?MemorySegment.NULL:resolvedBlobs());
        descriptor.set(ADDRESS,336,bindingCount==0?MemorySegment.NULL:bindings.segment);
        descriptor.set(ADDRESS,344,drawCount==0?MemorySegment.NULL:draws.segment);
        descriptor.set(JAVA_INT,352,blobCount); descriptor.set(JAVA_INT,356,bindingCount); descriptor.set(JAVA_INT,360,drawCount); descriptor.set(JAVA_INT,364,0);
        descriptor.set(ADDRESS,368,targetCount==0?MemorySegment.NULL:targets.segment);
        descriptor.set(JAVA_INT,376,targetCount); descriptor.set(JAVA_INT,380,0);
        descriptor.set(ADDRESS,384,effectCount==0?MemorySegment.NULL:effects.segment);
        descriptor.set(JAVA_INT,392,effectCount); descriptor.set(JAVA_INT,396,guiBlurSplit);
    }
    private final NativeFrameBuffer.Storage pointers=new NativeFrameBuffer.Storage();
    private MemorySegment resolvedBlobs() {
        pointers.reserve(blobCount*16L);
        for(int i=0;i<blobCount;i++) {
            long at=i*16L,size=blobs.segment.get(JAVA_LONG,at+8); var source=borrowed.get(i);
            if(source==null) source=bytes.segment.asSlice(blobs.segment.get(JAVA_LONG,at),size);
            pointers.segment.set(ADDRESS,at,source); pointers.segment.set(JAVA_LONG,at+8,size);
        }
        return pointers.segment;
    }
    @Override public void close() { bytes.close(); blobs.close(); bindings.close(); draws.close(); targets.close(); effects.close(); pointers.close(); borrowed.clear(); }
}
