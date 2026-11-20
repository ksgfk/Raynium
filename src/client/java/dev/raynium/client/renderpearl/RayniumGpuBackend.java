package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.device.*;
import dev.raynium.bridge.*;
import dev.raynium.client.terrain.BuiltinShaderPack;
import java.io.IOException;
import net.fabricmc.loader.api.FabricLoader;
import org.lwjgl.sdl.SDLVideo;
import org.slf4j.LoggerFactory;

public final class RayniumGpuBackend implements GpuBackend {
    private NativeEnvironment environment;
    @Override public String getName() { return "Raynium"; }
    @Override public void loadLibrary() throws BackendCreationException {
        var loader=FabricLoader.getInstance();
        try {
            var payload=new NativeLibraryLocator(loader.getGameDir(),loader.getModContainer("raynium").orElseThrow()).locate().orElseThrow();
            var api=NativeEnvironment.Backend.fromProperty(System.getProperty("raynium.rhi.backend","d3d12"));
            environment=new NativeEnvironment(payload,api,LoggerFactory.getLogger("raynium"));
        } catch(IOException | RuntimeException failure) {
            var error=new BackendCreationException("Cannot load Raynium: "+failure.getMessage(),BackendCreationException.Reason.PLATFORM_ERROR);
            error.initCause(failure);
            throw error;
        }
    }
    @Override public long createWindow(String title,int width,int height,long flags) {
        return SDLVideo.SDL_CreateWindow(title,width,height,flags & ~SDLVideo.SDL_WINDOW_VULKAN);
    }
    @Override public GpuDevice createDevice(GpuDebugOptions options) {
        var runtime=environment.createRuntime();
        try {
            var renderer=runtime.createRenderer(BuiltinShaderPack.load());
            try {
                var device=new RayniumGpuDevice(runtime,renderer,options.useLabels(),FabricLoader.getInstance().getGameDir());
                return device;
            }
            catch(RuntimeException | Error failure) { renderer.close(); throw failure; }
        } catch(RuntimeException | Error failure) { runtime.close(); throw failure; }
    }
    @Override public void unloadLibrary() { if(environment!=null) { environment.close(); environment=null; } }
}
