package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.util.List;

/** Physical exports map to logical textures; imported GPU work must finish before close. */
public final class NativeSharedTextures implements AutoCloseable {
    public record Texture(int width,int height,int mipLevels,boolean splitMips) {}
    public record Export(SharedImageInfo image,int textureIndex,int mipLevel,int mipCount) {}
    final NativeRenderer owner;
    MemorySegment handle=MemorySegment.NULL;
    List<Export> exports=List.of();
    NativeSharedTextures(NativeRenderer owner) { this.owner=owner; }
    public List<Export> exports() { checkOpen(); return exports; }
    public List<SharedImageInfo> images() { return exports().stream().map(Export::image).toList(); }
    private void checkOpen() {
        owner.checkOpen();
        if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Shared textures are closed");
    }
    @Override public void close() {
        owner.checkThread();
        if(handle.equals(MemorySegment.NULL)) return;
        NativeBindings.call(owner.bindings.destroySharedTextures,"destroy shared textures",handle);
        handle=MemorySegment.NULL; owner.textures=null;
    }
}
