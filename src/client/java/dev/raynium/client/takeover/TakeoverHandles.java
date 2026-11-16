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
            NativeTexture.Format nativeFormat=switch(format) {
                case RGBA8_UNORM -> NativeTexture.Format.RGBA8;
                case R8_UNORM -> NativeTexture.Format.R8;
                case RGBA16_FLOAT -> NativeTexture.Format.RGBA16F;
                case RGBA32_FLOAT -> NativeTexture.Format.RGBA32F;
                case D32_FLOAT -> NativeTexture.Format.DEPTH32F;
                default -> throw new IllegalArgumentException("Unsupported native texture format: "+format);
            };
            int flags=((usage&GpuTexture.USAGE_CUBEMAP_COMPATIBLE)!=0?NativeTexture.CUBE:0)
                |((usage&GpuTexture.USAGE_RENDER_ATTACHMENT)!=0?NativeTexture.ATTACHMENT:0)
                |((usage&GpuTexture.USAGE_COPY_SRC)!=0?NativeTexture.READBACK:0);
            nativeTexture=TakeoverRuntime.renderer().createTexture(width,height,mips,layers,nativeFormat,flags);
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
        View(GpuTexture texture,int base,int mips) {
            if(texture.isClosed() || base<0 || mips<=0 || base>texture.getMipLevels()-mips) throw new IllegalArgumentException("Invalid texture view range");
            this.texture=texture; this.base=base; this.mips=mips;
        }
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
            nativeTexture.uploadLayer(mip,layer,x,y,image.getWidth(),image.getHeight(),image.getWidth()*image.format().components(),MemorySegment.ofBuffer(image.getPixelBytes()));
        }
    }
    public static void clear(GpuTexture texture,Vector4fc color) {
        var nativeTexture=((Texture)texture).nativeTexture();
        for(int mip=0;mip<texture.getMipLevels();mip++) {
            int width=texture.getWidth(mip),height=texture.getHeight(mip);
            var pixels=MemorySegment.ofArray(clearPixels(nativeTexture.format(),width,height,color));
            for(int layer=0;layer<texture.getDepthOrLayers();layer++)
                nativeTexture.uploadLayer(mip,layer,0,0,width,height,Math.multiplyExact(width,nativeTexture.format().bytesPerPixel()),pixels);
        }
    }
    static byte[] clearPixels(NativeTexture.Format format,int width,int height,Vector4fc color) {
        if(width<=0 || height<=0 || format==NativeTexture.Format.DEPTH32F)
            throw new IllegalArgumentException("Color clear requires a nonempty color texture");
        var texel=ByteBuffer.allocate(format.bytesPerPixel()).order(java.nio.ByteOrder.nativeOrder());
        float[] values={color.x(),color.y(),color.z(),color.w()};
        for(int component=0;component<(format==NativeTexture.Format.R8?1:4);component++) {
            float value=values[component];
            switch(format) {
                case RGBA8,R8 -> texel.put((byte)Math.round(Math.clamp(value,0,1)*255));
                case RGBA16F -> texel.putShort(Float.floatToFloat16(value));
                case RGBA32F -> texel.putFloat(value);
                default -> throw new IllegalArgumentException("Unsupported clear format: "+format);
            }
        }
        byte[] pixels=new byte[Math.multiplyExact(Math.multiplyExact(width,height),format.bytesPerPixel())];
        System.arraycopy(texel.array(),0,pixels,0,texel.capacity());
        for(int filled=texel.capacity();filled<pixels.length;) {
            int copy=Math.min(filled,pixels.length-filled); System.arraycopy(pixels,0,pixels,filled,copy); filled+=copy;
        }
        return pixels;
    }
}
