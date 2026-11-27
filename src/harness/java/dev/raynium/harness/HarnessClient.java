package dev.raynium.harness;

import dev.raynium.harness.benchmark.BenchmarkSession;
import dev.raynium.harness.validation.VisualValidation;
import net.fabricmc.api.ClientModInitializer;
import org.slf4j.LoggerFactory;

public final class HarnessClient implements ClientModInitializer {
    @Override
    public void onInitializeClient() {
        boolean rayniumLoaded = RayniumHarnessBridge.isRayniumLoaded();
        VisualValidation.register(rayniumLoaded);
        BenchmarkSession.register(rayniumLoaded);
        if (!rayniumLoaded) {
            LoggerFactory.getLogger("raynium.harness").info("Vanilla reference renderer: Raynium core mod is not loaded");
        }
    }
}
