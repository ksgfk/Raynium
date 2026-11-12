package dev.raynium.client.takeover;

import com.mojang.renderpearl.api.device.*;
import dev.raynium.bridge.*;
import java.util.Set;
import org.lwjgl.sdl.SDLProperties;
import org.lwjgl.sdl.SDLVideo;
import org.slf4j.Logger;

public final class TakeoverRuntime {
    private static NativeEnvironment environment;
    private static NativeRuntime runtime;
    private static NativeRenderer renderer;
    private static NativeSwapchain swapchain;
    private static DeviceInfo info;
    private static Logger logger;
    private static long sequence;
    private static long frames,presented,skipped,levelFrames,lastStats=System.nanoTime(),lastStatsFrame,worldStarted;
    private static long rebuilds;
    private static final long STATS_SECONDS=Long.getLong("raynium.takeover.statsSeconds",5);
    private static final java.util.List<java.lang.management.BufferPoolMXBean> BUFFER_POOLS=
        java.lang.management.ManagementFactory.getPlatformMXBeans(java.lang.management.BufferPoolMXBean.class);
    private static NativeRenderer.Frame frame;
    private static TerrainFrameEvent frameEvent;
    private static long preparationStarted;
    private static GpuSurface.Configuration configuration;
    private static boolean suboptimal,levelFrame;
    private static final dev.raynium.client.terrain.TakeoverTerrain terrain=new dev.raynium.client.terrain.TakeoverTerrain();
    public interface FrameSource { void raynium$prepare(); }
    private TakeoverRuntime() {}
    public static void install(NativeEnvironment owner,Logger log) { environment=owner; logger=org.slf4j.LoggerFactory.getLogger("raynium.takeover"); }
    public static DeviceInfo deviceInfo() {
        if(info!=null) return info;
        runtime=environment.createRuntime();
        var nativeInfo=runtime.deviceInfo();
        var limits=new DeviceLimits(Math.max(1,(int)nativeInfo.maxAnisotropy()),Math.toIntExact(nativeInfo.uniformOffsetAlignment()),nativeInfo.maxTextureSize(),
            Long.MAX_VALUE,0,nativeInfo.maxColorAttachments(),0);
        var type=switch(nativeInfo.deviceType()) { case 1->DeviceType.INTEGRATED; case 2->DeviceType.DISCRETE; case 3->DeviceType.CPU; default->DeviceType.OTHER; };
        info=new DeviceInfo(nativeInfo.name(),nativeInfo.vendor(),nativeInfo.driver(),true,"Raynium",1,limits,
            new DeviceFeatures(false,false,false,false,false,false,false,false),Set.of(),new HintsAndWorkarounds(false,false,false,false),type);
        return info;
    }
    public static NativeRenderer renderer() {
        if(renderer==null) throw new IllegalStateException("takeover: Renderer not created");
        return renderer;
    }
    public static boolean ready() { return renderer!=null && swapchain!=null; }
    public static void createSurface(long window) {
        deviceInfo();
        renderer=runtime.createRenderer(dev.raynium.client.terrain.BuiltinShaderPack.load());
        long hwnd=SDLProperties.SDL_GetPointerProperty(SDLVideo.SDL_GetWindowProperties(window),SDLVideo.SDL_PROP_WINDOW_WIN32_HWND_POINTER,0);
        try(var stack=org.lwjgl.system.MemoryStack.stackPush()) {
            var width=stack.mallocInt(1); var height=stack.mallocInt(1);
            if(!SDLVideo.SDL_GetWindowSizeInPixels(window,width,height)) throw new IllegalStateException("takeover: Cannot query framebuffer size");
            swapchain=renderer.createSwapchain(hwnd,width.get(0),height.get(0),NativeSwapchain.PresentMode.FIFO);
            configuration=new GpuSurface.Configuration(width.get(0),height.get(0),GpuSurface.PresentMode.FIFO);
            var frame=new NativeRenderer.Frame(); frame.outputSequence=++sequence; frame.clearColor=new float[]{.05f,.05f,.07f,1};
            var result=renderer.present(swapchain,frame);
            if(result!=NativeSwapchain.Presentation.PRESENTED) throw new IllegalStateException("takeover: Initial frame was not presented: "+result);
            dev.raynium.client.terrain.TerrainCapture.activate(true);
        }
    }
    public static NativeFrameBuffer frameBuffer() { return renderer().frameBuffer(); }
    public static void beginFrame(net.minecraft.client.renderer.state.GameRenderState state) {
        frames++;
        levelFrame=state.shouldRenderLevel;
        frameEvent=TerrainFrameEvent.start();
        preparationStarted=System.nanoTime();
        frame=new NativeRenderer.Frame();
        frame.epoch=terrain.beginFrame(renderer());
        frame.outputSequence=++sequence;
        frame.guiWidth=(float)state.windowRenderState.width/state.windowRenderState.guiScale;
        frame.guiHeight=(float)state.windowRenderState.height/state.windowRenderState.guiScale;
        var color=state.guiRenderState.clearColorOverride;
        frame.clearColor=new float[]{color.x,color.y,color.z,color.w};
        frameBuffer().begin(0,0,0,0,0);
    }
    public static void prepareTerrain(net.minecraft.client.Minecraft client) { terrain.prepare(renderer,client,frame); }
    public static void present() {
        try {
            long nativeStarted=System.nanoTime();
            var outcome=renderer.presentPrepared(swapchain,frame);
            if(frameEvent!=null) {
                frameEvent.sequence=frame.outputSequence;
                frameEvent.preparation=nativeStarted-preparationStarted;
                frameEvent.ffm=System.nanoTime()-nativeStarted;
                frameEvent.visible=frameBuffer().visibleCount();
                frameEvent.end(); frameEvent.commit(); frameEvent=null;
            }
            terrain.commit();
            if(levelFrame) dev.raynium.client.benchmark.BenchmarkSession.nativeFrame();
            if(outcome==NativeSwapchain.Presentation.PRESENTED) presented++; else skipped++;
            statistics();
            suboptimal=outcome==NativeSwapchain.Presentation.RECREATE;
        } catch(RuntimeException | Error failure) {
            try { close(); } catch(RuntimeException | Error cleanup) { failure.addSuppressed(cleanup); }
            throw failure;
        }
    }
    public static long levelFrames() { return levelFrames; }
    public static long presented() { return presented; }
    public static long skipped() { return skipped; }
    public static void maintained() { levelFrames++; }
    private static void statistics() {
        long now=System.nanoTime();
        var client=net.minecraft.client.Minecraft.getInstance();
        if(client.level!=null && worldStarted==0) worldStarted=now;
        if(STATS_SECONDS<=0 || now-lastStats<STATS_SECONDS*1_000_000_000L) return;
        var dispatcher=client.levelRenderer.sectionRenderDispatcher();
        var memory=Runtime.getRuntime();
        long bufferBytes=BUFFER_POOLS.stream().mapToLong(java.lang.management.BufferPoolMXBean::getMemoryUsed).sum();
        logger.info("Raynium takeover stats frames={} presented={} skipped={} fps={} level={} screen={} overlay={} visibleRenderable={} compileQueue={} heapUsedMiB={} worldSeconds={} levelFrames={} rebuilds={} acquireTimeouts={} transientBlocks={} bufferPoolMiB={}",
            frames,presented,skipped,Math.round((frames-lastStatsFrame)*1_000_000_000.0/(now-lastStats)),client.level!=null,
            client.gui.screen()==null?"none":client.gui.screen().getClass().getSimpleName(),client.gui.overlay()==null?"none":client.gui.overlay().getClass().getSimpleName(),
            client.level==null?0:client.levelExtractor.countRenderedSections(),dispatcher==null?0:dispatcher.getCompileQueueSize(),
            (memory.totalMemory()-memory.freeMemory())/(1024*1024),worldStarted==0?0:(now-worldStarted)/1_000_000_000L,levelFrames,
            rebuilds,swapchain.acquireTimeouts(),((LevelMaintenance)client.levelRenderer).raynium$transientBlocks(),bufferBytes/(1024*1024));
        lastStats=now; lastStatsFrame=frames;
    }
    public static boolean suboptimal() { return suboptimal; }
    public static java.util.Optional<GpuSurface.Configuration> configuration() { return java.util.Optional.ofNullable(configuration); }
    public static void configure(GpuSurface.Configuration request) {
        if(request.width()>0 && request.height()>0) {
            swapchain.resize(request.width(),request.height(),request.presentMode()==GpuSurface.PresentMode.IMMEDIATE?NativeSwapchain.PresentMode.IMMEDIATE:NativeSwapchain.PresentMode.FIFO);
            rebuilds++;
            suboptimal=false;
        }
        configuration=request;
    }
    public static void close() {
        dev.raynium.client.terrain.TerrainCapture.activate(false);
        if(swapchain!=null) { swapchain.close(); swapchain=null; }
        if(renderer!=null) { renderer.close(); renderer=null; }
        if(runtime!=null) { runtime.close(); runtime=null; }
        if(environment!=null) { environment.close(); environment=null; }
    }
}
