package dev.raynium.client;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientLifecycleEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.CrashReport;
import net.minecraft.client.Minecraft;
import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeLibraryLocator;
import dev.raynium.bridge.NativeRuntime;
import dev.raynium.client.preview.PreviewHost;
import dev.raynium.client.preview.PreviewOverlay;
import java.io.IOException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class RayniumClient implements ClientModInitializer {
    private NativeLibraryLocator.Payload selected;
    private NativeEnvironment.Backend backend;
    private NativeEnvironment environment;
    private NativeRuntime runtime;
    private PreviewOverlay preview;

    @Override
    public void onInitializeClient() {
        Logger logger = LoggerFactory.getLogger("raynium");
        prepare(logger);
        ClientLifecycleEvents.CLIENT_STARTED.register(client -> start(client, logger));
        ClientLifecycleEvents.CLIENT_STOPPING.register(this::closeNative);
    }

    private void prepare(Logger logger) {
        FabricLoader loader = FabricLoader.getInstance();
        try {
            var mod = loader.getModContainer("raynium").orElseThrow();
            var payload = new NativeLibraryLocator(loader.getGameDir(), mod).locate();
            if (payload.isPresent()) {
                selected = payload.get();
                backend = NativeEnvironment.Backend.fromProperty(System.getProperty("raynium.rhi.backend", "d3d12"));
                boolean validation = Boolean.getBoolean("raynium.rhi.validation");
                environment = new NativeEnvironment(selected, backend, validation, logger);
                logger.info("Raynium native environment prepared before Minecraft graphics initialization");
            } else if (Boolean.getBoolean("raynium.native.required")) {
                throw new IllegalStateException("Native payload is required; run nativeStage or use the packaged native mod JAR");
            } else {
                logger.info("No native payload selected; use runClientNative to build and load it");
            }
        } catch (IOException | RuntimeException | LinkageError failure) {
            closeEnvironment();
            throw new IllegalStateException("Preparing Raynium before Minecraft graphics initialization", failure);
        }
    }

    private void closeEnvironment() {
        if (environment != null) {
            environment.close();
            environment = null;
        }
    }

    private void closeNative(Minecraft client) {
        if (preview != null) {
            preview.close();
            ((PreviewHost) client).raynium$setPreview(null);
            preview = null;
        }
        if (runtime != null) {
            runtime.close();
            runtime = null;
        }
        closeEnvironment();
    }

    private void start(Minecraft client, Logger logger) {
        if (selected == null) return;
        try {
            var host = PreviewOverlay.hostGpu(logger);
            runtime = environment.createRuntime(host);
            preview = new PreviewOverlay(runtime, logger);
            ((PreviewHost) client).raynium$setPreview(preview);
        } catch (RuntimeException | LinkageError failure) {
            try {
                closeNative(client);
            } catch (RuntimeException | LinkageError cleanupFailure) {
                failure.addSuppressed(cleanupFailure);
            }
            logger.error("Raynium RHI initialization failed: backend={}", backend.propertyValue(), failure);
            CrashReport report = CrashReport.forThrowable(failure, "Initializing Raynium RHI");
            var details = report.addCategory("Raynium runtime");
            details.setDetail("Requested backend", backend.propertyValue());
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
