package dev.raynium.client.takeover;

import com.mojang.jtracy.TracyClient;
import dev.raynium.bridge.NativeRenderer;
import java.nio.ByteBuffer;
import org.lwjgl.system.MemoryUtil;

/** Opt-in profiler images retain the original one-pending-image and frame-delay semantics. */
public final class TakeoverTracyImages {
    private static ByteBuffer pixels;
    private static long next=1L<<61,pending;
    private static int delay;
    private static boolean reported;
    private TakeoverTracyImages() {}
    public static void enable() {
        if(pixels!=null) throw new IllegalStateException("Tracy image capture already enabled");
        pixels=MemoryUtil.memAlloc(320*180*4);
        reported=false;
    }
    public static void beforeSubmit() {
        if(pixels==null) return;
        var renderer=TakeoverRuntime.renderer();
        if(pending!=0) {
            var completed=renderer.pollCapture(pending);
            if(completed.isEmpty()) return;
            var capture=completed.get();
            pixels.clear().put(capture.rgba()).flip();
            TracyClient.frameImage(pixels,capture.width(),capture.height(),delay,false);
            if(!reported) {
                org.slf4j.LoggerFactory.getLogger("raynium").info("Tracy image capture active: {}x{}, top row first",capture.width(),capture.height());
                reported=true;
            }
            pending=0;
        }
        if(next==(1L<<62)) throw new IllegalStateException("Tracy image sequence space exhausted");
        pending=next++; delay=0;
        renderer.requestCapture(pending,NativeRenderer.CAPTURE_TRACY,0,0,0);
    }
    public static void endFrame() {
        if(pixels==null) return;
        ++delay; TracyClient.markFrame();
    }
    public static void close() {
        if(pixels!=null) MemoryUtil.memFree(pixels);
        pixels=null; pending=0;
    }
}
