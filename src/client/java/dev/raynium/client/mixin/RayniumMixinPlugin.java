package dev.raynium.client.mixin;

import dev.raynium.client.RenderMode;
import java.util.List;
import java.util.Set;
import org.objectweb.asm.tree.ClassNode;
import org.spongepowered.asm.mixin.extensibility.IMixinConfigPlugin;
import org.spongepowered.asm.mixin.extensibility.IMixinInfo;

public final class RayniumMixinPlugin implements IMixinConfigPlugin {
    @Override public boolean shouldApplyMixin(String target,String mixin) {
        if(mixin.startsWith("dev.raynium.client.mixin.VisualValidation")) return System.getProperty("raynium.validation.config")!=null;
        if(mixin.equals("dev.raynium.client.mixin.BenchmarkReferenceTimerMixin")) return RenderMode.REFERENCE && System.getProperty("raynium.benchmark.config")!=null;
        return !RenderMode.REFERENCE || mixin.equals("dev.raynium.client.mixin.MinecraftBenchmarkMixin");
    }
    @Override public void onLoad(String packageName) {}
    @Override public String getRefMapperConfig() { return null; }
    @Override public void acceptTargets(Set<String> own,Set<String> others) {}
    @Override public List<String> getMixins() { return null; }
    @Override public void preApply(String target,ClassNode node,String mixin,IMixinInfo info) {}
    @Override public void postApply(String target,ClassNode node,String mixin,IMixinInfo info) {}
}
