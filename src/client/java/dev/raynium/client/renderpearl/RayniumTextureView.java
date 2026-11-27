package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import java.util.Objects;
public final class RayniumTextureView implements GpuTextureView {
    private final GpuTexture texture;
    private final int base, mips;
    private boolean closed;
    public RayniumTextureView(GpuTexture texture, int base, int mips) {
        if (texture.isClosed() || base < 0 || mips <= 0 || base > texture.getMipLevels() - mips)
            throw new IllegalArgumentException("Invalid texture view range");
        this.texture = texture;
        this.base = base;
        this.mips = mips;
    }
    @Override
    public boolean isClosed() {
        return closed || texture.isClosed();
    }
    @Override
    public GpuTexture texture() {
        return texture;
    }
    @Override
    public int baseMipLevel() {
        return base;
    }
    @Override
    public int mipLevels() {
        return mips;
    }
    @Override
    public int getWidth(int mip) {
        return texture.getWidth(base + Objects.checkIndex(mip, mips));
    }
    @Override
    public int getHeight(int mip) {
        return texture.getHeight(base + Objects.checkIndex(mip, mips));
    }
    @Override
    public void close() {
        closed = true;
    }
}
