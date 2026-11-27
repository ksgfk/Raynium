package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.device.BackendCreationException;
import com.mojang.renderpearl.api.device.GpuBackend;
import com.mojang.renderpearl.api.device.GpuDebugOptions;
import com.mojang.renderpearl.api.device.GpuDevice;
import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeLibraryLocator;
import dev.raynium.client.config.RayniumConfig;
import java.io.IOException;
import net.fabricmc.loader.api.FabricLoader;
import org.lwjgl.sdl.SDLVideo;
import org.slf4j.LoggerFactory;

public final class RayniumGpuBackend implements GpuBackend {
    private RayniumConfig config;
    private NativeEnvironment environment;

    @Override
    public String getName() {
        return "Raynium";
    }

    @Override
    public void loadLibrary() throws BackendCreationException {
        var loader = FabricLoader.getInstance();
        var gameDir = loader.getGameDir();
        try {
            config = RayniumConfig.load(gameDir);
            var payload = new NativeLibraryLocator(gameDir, loader.getModContainer("raynium").orElseThrow()).locate().orElseThrow();
            environment = new NativeEnvironment(payload, config.backend(), config.rhiValidation(), LoggerFactory.getLogger("raynium"));
        } catch (IOException | RuntimeException failure) {
            config = null;
            var error = new BackendCreationException("Cannot load Raynium: " + failure.getMessage(), BackendCreationException.Reason.PLATFORM_ERROR);
            error.initCause(failure);
            throw error;
        }
    }

    @Override
    public long createWindow(String title, int width, int height, long flags) {
        return SDLVideo.SDL_CreateWindow(title, width, height, flags & ~SDLVideo.SDL_WINDOW_VULKAN);
    }

    @Override
    public GpuDevice createDevice(GpuDebugOptions options) {
        var runtime = environment.createRuntime();
        try {
            var renderer = runtime.createRenderer(BuiltinShaderPack.load(), config.terrainSubmission());
            try {
                return new RayniumGpuDevice(runtime, renderer, options.useLabels(), FabricLoader.getInstance().getGameDir(), config.shaderToolsDir());
            } catch (RuntimeException | Error failure) {
                renderer.close();
                throw failure;
            }
        } catch (RuntimeException | Error failure) {
            runtime.close();
            throw failure;
        }
    }

    @Override
    public void unloadLibrary() {
        config = null;
        if (environment != null) {
            environment.close();
            environment = null;
        }
    }
}
