package dev.raynium.client.takeover;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.platform.NativeImage;
import dev.raynium.bridge.NativeRenderer;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import net.minecraft.client.Minecraft;

/** Requests own CPU callbacks; the native renderer owns readbacks until their completion is polled. */
public final class TakeoverScreenshots {
    private record Request(int scale,Consumer<NativeImage> callback) {}
    private record Pending(long sequence,Consumer<NativeRenderer.Capture> callback) {}
    private static final List<Pending> pending=new ArrayList<>();
    private static long sequence=1L<<62;
    private static boolean worldCapture;
    private TakeoverScreenshots() {}
    public static void atWorldStage(Runnable action) {
        boolean previous=worldCapture; worldCapture=true;
        try { action.run(); } finally { worldCapture=previous; }
    }
    public static void request(RenderTarget target,int scale,Consumer<NativeImage> callback) {
        if(target!=Minecraft.getInstance().gameRenderer.mainRenderTarget())
            throw new IllegalArgumentException("Screenshot target is not the native main output");
        if(scale<=0 || target.width%scale!=0 || target.height%scale!=0)
            throw new IllegalArgumentException("Image size is not divisible by downscale factor");
        var request=new Request(scale,java.util.Objects.requireNonNull(callback));
        enqueue(worldCapture?NativeRenderer.CAPTURE_WORLD:NativeRenderer.CAPTURE_LAST,0,0,0,capture->deliver(capture,request));
    }
    public static void texture(dev.raynium.bridge.NativeTexture texture,int mip,int layer,Consumer<NativeRenderer.Capture> callback) {
        enqueue(NativeRenderer.CAPTURE_TEXTURE,texture.id(),mip,layer,callback);
    }
    private static void enqueue(int source,int texture,int mip,int layer,Consumer<NativeRenderer.Capture> callback) {
        if(sequence==Long.MAX_VALUE) throw new IllegalStateException("Capture ID space exhausted");
        long id=sequence++;
        TakeoverRuntime.renderer().requestCapture(id,source,texture,mip,layer);
        pending.add(new Pending(id,java.util.Objects.requireNonNull(callback)));
    }
    public static boolean hasRequests() { return !pending.isEmpty(); }
    public static void beforePresent(NativeRenderer renderer,NativeRenderer.Frame frame) { poll(renderer); }
    public static void poll(NativeRenderer renderer) {
        for(var iterator=pending.iterator();iterator.hasNext();) {
            var item=iterator.next();
            var result=renderer.pollCapture(item.sequence());
            if(result.isEmpty()) continue;
            iterator.remove();
            net.minecraft.util.Util.ioPool().execute(()->item.callback().accept(result.get()));
        }
    }
    private static void deliver(NativeRenderer.Capture capture,Request request) {
        int scale=request.scale(),width=capture.width()/scale,height=capture.height()/scale;
        if(capture.width()%scale!=0 || capture.height()%scale!=0)
            throw new IllegalArgumentException("Captured image size is not divisible by downscale factor");
        NativeImage image=new NativeImage(width,height,false);
        try {
            var destination=image.getPixelBytes(); byte[] pixels=capture.rgba();
            for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
                long red=0,green=0,blue=0;
                for(int j=0;j<scale;j++) for(int i=0;i<scale;i++) {
                    int source=((y*scale+j)*capture.width()+x*scale+i)*4;
                    red+=pixels[source]&255; green+=pixels[source+1]&255; blue+=pixels[source+2]&255;
                }
                long count=(long)scale*scale; int offset=(y*width+x)*4;
                destination.put(offset,(byte)(red/count)); destination.put(offset+1,(byte)(green/count));
                destination.put(offset+2,(byte)(blue/count)); destination.put(offset+3,(byte)255);
            }
            request.callback().accept(image);
        } catch(RuntimeException | Error failure) { image.close(); throw failure; }
    }
    public static void clear() { pending.clear(); }
}
