package dev.raynium.client.takeover.scene;

import com.mojang.blaze3d.platform.Lighting;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.raynium.client.takeover.TakeoverHandles;
import dev.raynium.client.takeover.TakeoverRuntime;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;

public final class SceneOffscreen {
    private SceneOffscreen() {}
    private static Lighting.Entry lighting=Lighting.Entry.ITEMS_3D;
    public static void lighting(Lighting.Entry entry) { lighting=entry; }
    public static Lighting.Entry lighting() { return lighting; }
    public static void capture(GpuTexture texture,int x,int y,int width,int height,SubmitNodeStorage storage,
            FeatureRenderDispatcher dispatcher,Matrix4fc view) {
        var frame=TakeoverRuntime.frameBuffer().scene();
        frame.beginTarget(((TakeoverHandles.Texture)texture).nativeTexture().id(),x,y,width,height);
        try {
            // Vanilla offscreen blits sample bottom-to-top; render the tile in that texture orientation.
            ((SceneFeatureSource)dispatcher).raynium$captureOffscreen(storage,new Matrix4f().setOrtho(0,width,0,height,1000,-1000,true),view,width,height);
        } finally { frame.endTarget(); }
    }
    /** Logical depth identity for original close paths; each update gets its depth attachment from the RDG pool. */
    public static final class Depth implements GpuTexture {
        private final int width,height; private boolean closed;
        public Depth(int width,int height) { this.width=width; this.height=height; }
        @Override public int getWidth(int mip) { return width; }
        @Override public int getHeight(int mip) { return height; }
        @Override public int getDepthOrLayers() { return 1; }
        @Override public int getMipLevels() { return 1; }
        @Override public GpuFormat getFormat() { return GpuFormat.D32_FLOAT; }
        @Override public int usage() { return GpuTexture.USAGE_RENDER_ATTACHMENT; }
        @Override public String getLabel() { return "RDG offscreen depth"; }
        @Override public boolean isClosed() { return closed; }
        @Override public void close() { closed=true; }
    }
}
