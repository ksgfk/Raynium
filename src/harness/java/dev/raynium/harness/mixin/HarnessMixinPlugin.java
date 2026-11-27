package dev.raynium.harness.mixin;

import java.util.List;
import java.util.Set;
import net.fabricmc.loader.api.FabricLoader;
import org.objectweb.asm.tree.ClassNode;
import org.spongepowered.asm.mixin.extensibility.IMixinConfigPlugin;
import org.spongepowered.asm.mixin.extensibility.IMixinInfo;

public final class HarnessMixinPlugin implements IMixinConfigPlugin {
    @Override
    public boolean shouldApplyMixin(String target, String mixin) {
        if (mixin.startsWith("dev.raynium.harness.mixin.validation.")) {
            return System.getProperty("raynium.validation.config") != null;
        }
        if (mixin.equals("dev.raynium.harness.mixin.benchmark.BenchmarkReferenceTimerMixin")) {
            return !FabricLoader.getInstance().isModLoaded("raynium") && System.getProperty("raynium.benchmark.config") != null;
        }
        if (mixin.startsWith("dev.raynium.harness.mixin.benchmark.")) {
            return System.getProperty("raynium.benchmark.config") != null;
        }
        return false;
    }

    @Override
    public void onLoad(String packageName) {
    }

    @Override
    public String getRefMapperConfig() {
        return null;
    }

    @Override
    public void acceptTargets(Set<String> own, Set<String> others) {
    }

    @Override
    public List<String> getMixins() {
        return null;
    }

    @Override
    public void preApply(String target, ClassNode node, String mixin, IMixinInfo info) {
    }

    @Override
    public void postApply(String target, ClassNode node, String mixin, IMixinInfo info) {
    }
}
