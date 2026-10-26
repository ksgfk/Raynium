package dev.raynium.client;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientLifecycleEvents;
import net.fabricmc.loader.api.FabricLoader;
import dev.raynium.bridge.NativeLibrary;
import dev.raynium.bridge.NativeLibraryLocator;
import java.io.IOException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class RayniumClient implements ClientModInitializer {
    private NativeLibrary nativeLibrary;

    @Override
    public void onInitializeClient() {
        Logger logger = LoggerFactory.getLogger("raynium");
        FabricLoader loader = FabricLoader.getInstance();
        var mod = loader.getModContainer("raynium").orElseThrow();
        try {
            var payload = new NativeLibraryLocator(loader.getGameDir(), mod).locate();
            if (payload.isPresent()) {
                nativeLibrary = new NativeLibrary(payload.get(), logger);
                ClientLifecycleEvents.CLIENT_STOPPING.register(client -> {
                    if (nativeLibrary != null) {
                        nativeLibrary.close();
                        nativeLibrary = null;
                        logger.info("Raynium native library closed");
                    }
                });
            } else if (Boolean.getBoolean("raynium.native.required")) {
                throw new IllegalStateException("Native payload is required; run nativeStage or use the packaged native mod JAR");
            } else {
                logger.info("No native payload selected; use runClientNative to build and load it");
            }
        } catch (IOException failure) {
            throw new IllegalStateException("Cannot prepare the Raynium native library: " + failure.getMessage(), failure);
        }
        logger.info("Raynium initialized; vanilla rendering is active.");
    }
}
