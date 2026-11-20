package dev.raynium.client;

import dev.raynium.client.benchmark.BenchmarkSession;
import net.fabricmc.api.ClientModInitializer;
import org.slf4j.LoggerFactory;

public final class RayniumClient implements ClientModInitializer {
    @Override public void onInitializeClient() {
        var logger=LoggerFactory.getLogger("raynium");
        dev.raynium.client.validation.VisualValidation.register();
        if(RenderMode.REFERENCE) {
            BenchmarkSession.register();
            logger.info("Vanilla reference renderer: native payload and rendering takeover disabled");
            return;
        }
        BenchmarkSession.register();
    }
}
