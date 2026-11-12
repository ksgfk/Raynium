package dev.raynium.client;

import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeLibraryLocator;
import dev.raynium.client.benchmark.BenchmarkSession;
import dev.raynium.client.takeover.TakeoverRuntime;
import java.io.IOException;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.loader.api.FabricLoader;
import org.slf4j.LoggerFactory;

public final class RayniumClient implements ClientModInitializer {
    @Override public void onInitializeClient() {
        var logger=LoggerFactory.getLogger("raynium");
        NativeEnvironment environment=null;
        try {
            var loader=FabricLoader.getInstance();
            var mod=loader.getModContainer("raynium").orElseThrow();
            var payload=new NativeLibraryLocator(loader.getGameDir(),mod).locate().orElseThrow(
                ()->new IllegalStateException("Native payload is required; use runClientNative or the packaged native mod JAR"));
            var backend=NativeEnvironment.Backend.fromProperty(System.getProperty("raynium.rhi.backend","d3d12"));
            environment=new NativeEnvironment(payload,backend,Boolean.getBoolean("raynium.rhi.validation"),logger);
            TakeoverRuntime.install(environment,logger);
            BenchmarkSession.register();
            logger.info("Raynium native environment prepared before Minecraft graphics initialization");
        } catch(IOException | RuntimeException | LinkageError failure) {
            if(environment!=null) {
                try { environment.close(); }
                catch(RuntimeException | LinkageError cleanup) { failure.addSuppressed(cleanup); }
            }
            throw new IllegalStateException("Preparing Raynium before Minecraft graphics initialization",failure);
        }
    }
}
