package dev.raynium.client.terrain;
import net.minecraft.client.renderer.texture.TextureAtlas;
import java.util.*;

/** Render-thread dirty regions include sprite padding and coalesce repeated ticks. */
public final class TerrainTextures {
    public record Rect(int x,int y,int width,int height) {}
    private static TextureAtlas atlas;
    private static final Set<Rect> dirty=new LinkedHashSet<>();
    private static boolean full=true,light=true;
    public static void uploaded(TextureAtlas value) {
        if(!value.location().equals(TextureAtlas.LOCATION_BLOCKS)) return;
        atlas=value; dirty.clear(); full=true; TerrainCapture.invalidate();
    }
    public static TextureAtlas atlas() { return atlas; }
    public static void dirty(Rect rect) { dirty.add(rect); }
    public static void light() { light=true; }
    public static List<Rect> drain(boolean force) {
        if(atlas==null) return List.of();
        var texture=atlas.getTexture();
        var result=full || force ? List.of(new Rect(0,0,texture.getWidth(0),texture.getHeight(0))) : List.copyOf(dirty);
        dirty.clear(); full=false; return result;
    }
    public static boolean drainLight(boolean force) { boolean result=light || force; light=false; return result; }
}
