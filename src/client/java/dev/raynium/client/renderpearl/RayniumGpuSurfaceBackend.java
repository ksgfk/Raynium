package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.device.GpuSurface;
import com.mojang.renderpearl.api.device.SurfaceException;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.renderpearl.backend.api.CommandEncoderBackend;
import com.mojang.renderpearl.backend.api.GpuSurfaceBackend;
import com.mojang.renderpearl.frontend.FrontendGpuSurface;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.bridge.NativeSwapchain;
import dev.raynium.bridge.NativeTexture;
import java.util.Collection;
import java.util.List;
import java.util.function.BooleanSupplier;
import org.lwjgl.sdl.SDLProperties;
import org.lwjgl.sdl.SDLVideo;

final class RayniumGpuSurfaceBackend implements GpuSurfaceBackend {
    private final NativeRenderer renderer;
    private final long hwnd;
    private final FrontendGpuSurface frontend = new FrontendGpuSurface(this);
    private NativeSwapchain swapchain;
    private boolean suboptimal,recreate,closed;
    private SurfaceException deferred;
    private boolean submittedBlit;

    RayniumGpuSurfaceBackend(NativeRenderer renderer,long window,BooleanSupplier iconified) {
        this.renderer=renderer;
        hwnd=SDLProperties.SDL_GetPointerProperty(SDLVideo.SDL_GetWindowProperties(window),SDLVideo.SDL_PROP_WINDOW_WIN32_HWND_POINTER,0);
        if(hwnd==0) throw new IllegalArgumentException("SDL window has no HWND");
    }
    FrontendGpuSurface frontend() {
        return frontend;
    }
    @Override public void configure(GpuSurface.Configuration configuration) throws SurfaceException {
        if(closed || configuration.width()<=0 || configuration.height()<=0) throw new SurfaceException("Invalid surface configuration");
        var mode=configuration.presentMode()==GpuSurface.PresentMode.FIFO?NativeSwapchain.PresentMode.FIFO:NativeSwapchain.PresentMode.IMMEDIATE;
        try {
            if(recreate && swapchain!=null) { swapchain.close(); swapchain=null; }
            if(swapchain==null) {
                swapchain=renderer.createSwapchain(hwnd,configuration.width(),configuration.height(),mode);
                renderer.onTerminalSurface(this::releaseAcquisition);
            } else swapchain.resize(configuration.width(),configuration.height(),mode);
        } catch(IllegalStateException failed) { throw new SurfaceException(failed); }
        suboptimal=recreate=submittedBlit=false;
        deferred=null;
    }
    @Override public Collection<GpuSurface.PresentMode> supportedPresentModes() {
        return List.of(GpuSurface.PresentMode.FIFO,GpuSurface.PresentMode.IMMEDIATE);
    }
    private void releaseAcquisition() {
        frontend.hasImageAcquired=false;
        frontend.hasBlittedTexture=false;
        submittedBlit=false;
    }
    @Override public boolean isSuboptimal() { return suboptimal; }
    @Override public void acquireNextTexture() throws SurfaceException {
        renderer.checkOpen();
        if(deferred!=null) { var error=deferred; deferred=null; throw error; }
        if(swapchain==null || closed) throw new SurfaceException("Surface currently unavailable");
        submittedBlit=false;
    }
    @Override public void blitFromTexture(CommandEncoderBackend encoder,GpuTextureView view) {
        if(!(encoder instanceof RayniumCommandEncoderBackend backend) || !(view.texture() instanceof NativeTexture texture) || view.isClosed())
            throw new IllegalArgumentException("Invalid surface blit source");
        backend.tape().blitToSurface(texture,view.baseMipLevel(),swapchain,this::presented);
    }
    private void presented(int outcome) {
        submittedBlit=true;
        if((outcome==0 || outcome==4) && dev.raynium.client.benchmark.BenchmarkSession.isActive() &&
           net.minecraft.client.Minecraft.getInstance().level!=null) dev.raynium.client.benchmark.BenchmarkSession.nativeFrame();
        switch(outcome) {
            case 0,1 -> {}
            case 4 -> suboptimal=true;
            case 2,5 -> {
                suboptimal=true;
                recreate=outcome==5;
                deferred=new SurfaceException(recreate?"Surface lost":"Swapchain out of date");
            }
            default -> throw new IllegalStateException("Unexpected surface presentation result "+outcome);
        }
    }
    @Override public void present() {
        if(!submittedBlit) throw new IllegalStateException("Blit batch has not submitted");
        submittedBlit=false;
    }
    @Override public void close() {
        if(closed) return;
        if(swapchain!=null) {
            swapchain.close();
            renderer.onTerminalSurface(null);
        }
        closed=true;
    }
}
