package dev.raynium.harness.mixin.benchmark;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.raynium.harness.RayniumHarnessBridge;
import dev.raynium.harness.benchmark.BenchmarkSession;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.debug.DebugScreenEntryList;
import net.minecraft.resources.Identifier;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
abstract class BenchmarkMinecraftMixin {
    @WrapOperation(
        method = "renderFrame",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/gui/components/debug/DebugScreenEntryList;isCurrentlyEnabled(Lnet/minecraft/resources/Identifier;)Z"))
    private boolean
    rayniumHarness$referenceTiming(DebugScreenEntryList entries, Identifier id, Operation<Boolean> original) {
        return (!RayniumHarnessBridge.isRayniumLoaded() && BenchmarkSession.isActive()) || original.call(entries, id);
    }

    @Inject(method = "renderFrame", at = @At("HEAD"))
    private void rayniumHarness$beforeBenchmarkFrame(CallbackInfo callback) {
        BenchmarkSession.beforeFrame((Minecraft)(Object)this);
    }

    @Inject(method = "renderFrame", at = @At("RETURN"))
    private void rayniumHarness$afterBenchmarkFrame(CallbackInfo callback) {
        if (!RayniumHarnessBridge.isRayniumLoaded() && ((Minecraft)(Object)this).level != null) {
            BenchmarkSession.nativeFrame();
        }
        BenchmarkSession.afterFrame();
    }
}
