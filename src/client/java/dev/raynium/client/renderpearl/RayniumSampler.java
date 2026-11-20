package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.textures.*;
import java.util.OptionalDouble;

public final class RayniumSampler implements GpuSampler {
    private final AddressMode u,v; private final FilterMode min,mag; private final int anisotropy; private final OptionalDouble lod; private boolean closed;
    public RayniumSampler(AddressMode u,AddressMode v,FilterMode min,FilterMode mag,int anisotropy,OptionalDouble lod) {
        this.u=u; this.v=v; this.min=min; this.mag=mag; this.anisotropy=anisotropy; this.lod=lod;
    }
    static int flags(GpuSampler sampler) {
        return (sampler.getAddressModeU()==AddressMode.REPEAT?1:0)|(sampler.getAddressModeV()==AddressMode.REPEAT?2:0)|
            (sampler.getMinFilter()==FilterMode.LINEAR?4:0)|(sampler.getMagFilter()==FilterMode.LINEAR?8:0)|
            (sampler.getMaxLod().orElse(1000)>0.25?16:32);
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
