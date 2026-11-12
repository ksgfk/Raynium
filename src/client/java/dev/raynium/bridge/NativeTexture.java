package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import static java.lang.foreign.ValueLayout.*;

/** Renderer-owned texture. Upload copies caller bytes before returning; IDs are never reused. */
public final class NativeTexture implements AutoCloseable {
    public enum Format { RGBA8(4), R8(1); private final int bytes; Format(int bytes) { this.bytes=bytes; } public int bytesPerPixel() { return bytes; } }
    private final NativeRenderer owner;
    private final int id,width,height,mips;
    private final Format format;
    private boolean closed;
    NativeTexture(NativeRenderer owner,int id,int width,int height,int mips,Format format) {
        this.owner=owner; this.id=id; this.width=width; this.height=height; this.mips=mips; this.format=format;
    }
    public int id() { checkOpen(); return id; }
    public int width() { return width; }
    public int height() { return height; }
    public int mipLevels() { return mips; }
    public Format format() { return format; }
    public void upload(int mip,int x,int y,int width,int height,int rowPitch,MemorySegment pixels) {
        checkOpen();
        try(var arena=Arena.ofConfined()) {
            if(!pixels.isNative()) {
                var copied=arena.allocate(pixels.byteSize(),1);
                copied.copyFrom(pixels); pixels=copied;
            }
            var desc=arena.allocate(48,8); desc.set(JAVA_INT,0,48); desc.set(JAVA_INT,4,id);
            desc.set(JAVA_INT,8,mip); desc.set(JAVA_INT,12,x); desc.set(JAVA_INT,16,y); desc.set(JAVA_INT,20,width);
            desc.set(JAVA_INT,24,height); desc.set(JAVA_INT,28,rowPitch); desc.set(ADDRESS,32,pixels); desc.set(JAVA_LONG,40,pixels.byteSize());
            owner.uploadTexture(desc);
        }
    }
    public void upload(int mip,int x,int y,int width,int height,byte[] pixels) {
        upload(mip,x,y,width,height,Math.multiplyExact(width,format.bytesPerPixel()),MemorySegment.ofArray(pixels));
    }
    private void checkOpen() { owner.checkOpen(); if(closed) throw new IllegalStateException("Texture is closed"); }
    @Override public void close() {
        owner.checkThread(); if(closed) return;
        if(!owner.isClosed()) owner.destroyTexture(id);
        closed=true;
    }
}
