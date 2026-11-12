package dev.raynium.client.takeover;

import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.bridge.NativeTexture;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.util.OptionalDouble;
import java.util.function.Supplier;
import org.joml.Vector4fc;

public final class TakeoverHandles {
    private TakeoverHandles() {}
    public static final class Texture implements GpuTexture {
        private final String label;
        private final int usage,width,height,layers,mips;
        private final GpuFormat format;
        private final NativeTexture nativeTexture;
        private boolean closed;
        Texture(String label,int usage,GpuFormat format,int width,int height,int layers,int mips) {
            this.label=label; this.usage=usage; this.format=format; this.width=width; this.height=height; this.layers=layers; this.mips=mips;
            nativeTexture=layers==1 && (format==GpuFormat.RGBA8_UNORM || format==GpuFormat.R8_UNORM)?
                TakeoverRuntime.renderer().createTexture(width,height,mips,format==GpuFormat.R8_UNORM?NativeTexture.Format.R8:NativeTexture.Format.RGBA8):null;
        }
        public NativeTexture nativeTexture() { if(closed) throw new IllegalStateException("takeover: Texture closed"); return nativeTexture; }
        @Override public int getWidth(int mip) { return Math.max(1,width>>mip); }
        @Override public int getHeight(int mip) { return Math.max(1,height>>mip); }
        @Override public int getDepthOrLayers() { return layers; }
        @Override public int getMipLevels() { return mips; }
        @Override public GpuFormat getFormat() { return format; }
        @Override public int usage() { return usage; }
        @Override public String getLabel() { return label; }
        @Override public boolean isClosed() { return closed; }
        @Override public void close() { if(!closed) { if(nativeTexture!=null) nativeTexture.close(); closed=true; } }
    }
    public static final class View implements GpuTextureView {
        private final GpuTexture texture; private final int base,mips; private boolean closed;
        View(GpuTexture texture,int base,int mips) { this.texture=texture; this.base=base; this.mips=mips; }
        @Override public boolean isClosed() { return closed || texture.isClosed(); }
        @Override public GpuTexture texture() { return texture; }
        @Override public int baseMipLevel() { return base; }
        @Override public int mipLevels() { return mips; }
        @Override public int getWidth(int mip) { return texture.getWidth(base+mip); }
        @Override public int getHeight(int mip) { return texture.getHeight(base+mip); }
        @Override public void close() { closed=true; }
    }
    public static final class Buffer implements GpuBuffer {
        private final int usage; private final long size; private boolean closed;
        Buffer(int usage,long size) { this.usage=usage; this.size=size; }
        @Override public long size() { return size; }
        @Override public int usage() { return usage; }
        @Override public boolean isClosed() { return closed; }
        @Override public void close() { closed=true; }
        @Override public GpuBufferSlice.MappedView map(long offset,long length,boolean read,boolean write) { throw new IllegalStateException("takeover: Identity buffer mapping is unsupported"); }
    }
    public static final class Sampler implements GpuSampler {
        private final AddressMode u,v; private final FilterMode min,mag; private final int anisotropy; private final OptionalDouble lod; private boolean closed;
        public Sampler(AddressMode u,AddressMode v,FilterMode min,FilterMode mag,int anisotropy,OptionalDouble lod) {
            this.u=u; this.v=v; this.min=min; this.mag=mag; this.anisotropy=anisotropy; this.lod=lod;
        }
        @Override public AddressMode getAddressModeU() { return u; }
        @Override public AddressMode getAddressModeV() { return v; }
        @Override public FilterMode getMinFilter() { return min; }
        @Override public FilterMode getMagFilter() { return mag; }
        @Override public int getMaxAnisotropy() { return anisotropy; }
        @Override public OptionalDouble getMaxLod() { return lod; }
        @Override public boolean isClosed() { return closed; }
        @Override public void close() { closed=true; }
    }
    public static GpuBuffer buffer(Supplier<String> label,int usage,long size) { return new Buffer(usage,size); }
    public static GpuBuffer buffer(Supplier<String> label,int usage,ByteBuffer data) { return new Buffer(usage,data.remaining()); }
    public static GpuTexture texture(String label,int usage,GpuFormat format,int width,int height,int layers,int mips) {
        return new Texture(label,usage,format,width,height,layers,mips);
    }
    public static GpuTextureView view(GpuTexture texture,int base,int mips) { return new View(texture,base,mips); }
    public static void write(GpuTexture texture,NativeImage image) {
        write(texture,image,0,0,0,0);
    }
    public static void write(GpuTexture texture,NativeImage image,int mip,int layer,int x,int y) {
        var nativeTexture=((Texture)texture).nativeTexture();
        if(nativeTexture!=null) {
            if(layer!=0) throw new IllegalArgumentException("takeover: Uploaded texture array layer is unsupported");
            nativeTexture.upload(mip,x,y,image.getWidth(),image.getHeight(),image.getWidth()*image.format().components(),MemorySegment.ofBuffer(image.getPixelBytes()));
        }
    }
    public static void clear(GpuTexture texture,Vector4fc color) {
        var nativeTexture=((Texture)texture).nativeTexture();
        if(nativeTexture==null) return;
        byte[] pixels=new byte[Math.multiplyExact(texture.getWidth(0)*texture.getHeight(0),4)];
        byte[] rgba={(byte)Math.round(color.x()*255),(byte)Math.round(color.y()*255),(byte)Math.round(color.z()*255),(byte)Math.round(color.w()*255)};
        for(int p=0;p<pixels.length;p++) pixels[p]=rgba[p&3];
        nativeTexture.upload(0,0,0,texture.getWidth(0),texture.getHeight(0),pixels);
    }
}
