package dev.raynium.client;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientLifecycleEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.CrashReport;
import net.minecraft.client.Minecraft;
import dev.raynium.bridge.NativeLibraryLocator;
import dev.raynium.bridge.NativeRuntime;
import java.io.IOException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class RayniumClient implements ClientModInitializer {
    private NativeRuntime runtime;

    @Override
    public void onInitializeClient() {
        Logger logger = LoggerFactory.getLogger("raynium");
        ClientLifecycleEvents.CLIENT_STARTED.register(client -> start(client, logger));
        ClientLifecycleEvents.CLIENT_STOPPING.register(client -> {
            if (runtime != null) {
                runtime.close();
                runtime = null;
            }
        });
    }

    private void start(Minecraft client, Logger logger) {
        FabricLoader loader = FabricLoader.getInstance();
        String requestedBackend = System.getProperty("raynium.rhi.backend", "d3d12");
        NativeLibraryLocator.Payload selected = null;
        try {
            var mod = loader.getModContainer("raynium").orElseThrow();
            var payload = new NativeLibraryLocator(loader.getGameDir(), mod).locate();
            if (payload.isPresent()) {
                selected = payload.get();
                runtime = new NativeRuntime(selected, NativeRuntime.Backend.fromProperty(requestedBackend), logger);
            } else if (Boolean.getBoolean("raynium.native.required")) {
                throw new IllegalStateException("Native payload is required; run nativeStage or use the packaged native mod JAR");
            } else {
                logger.info("No native payload selected; use runClientNative to build and load it");
            }
        } catch (IOException | RuntimeException | LinkageError failure) {
            logger.error("Raynium RHI initialization failed: backend={}", requestedBackend, failure);
            CrashReport report = CrashReport.forThrowable(failure, "Initializing Raynium RHI");
            var details = report.addCategory("Raynium runtime");
            details.setDetail("Requested backend", requestedBackend);
            details.setDetail("Native directory", System.getProperty("raynium.native.dir", "embedded payload"));
            if (selected != null) {
                details.setDetail("Native library", selected.libraryPath().toString());
                details.setDetail("Native source", selected.source());
                details.setDetail("Native build", selected.manifest().buildId());
                details.setDetail("RadRay revision", selected.manifest().radrayCommit());
            }
            // CLIENT_STARTED executes before Minecraft.run's crash handler.
            client.emergencySaveAndCrash(report);
        }
    }
}
