package dev.raynium.harness.mixin.benchmark;

import com.mojang.blaze3d.systems.TimerQuery;
import dev.raynium.harness.benchmark.BenchmarkSession;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(TimerQuery.class)
abstract class BenchmarkReferenceTimerMixin {
    @Shadow
    private TimerQuery.Status status;
    @Shadow
    private int currentRotationIndex;
    @Shadow
    @Final
    private long[] results;
    @Unique
    private final boolean[] rayniumHarness$measured = new boolean[3];
    @Unique
    private boolean rayniumHarness$pending;
    @Inject(method = "beginProfile", at = @At("RETURN"))
    private void rayniumHarness$started(CallbackInfo callback) {
        rayniumHarness$measured[currentRotationIndex] = BenchmarkSession.isMeasuring();
    }
    @Inject(method = "getStatus", at = @At("HEAD"))
    private void rayniumHarness$polling(CallbackInfoReturnable<TimerQuery.Status> callback) {
        rayniumHarness$pending = status == TimerQuery.Status.AWAITING_VALUES;
    }
    @Inject(method = "getStatus", at = @At("RETURN"))
    private void rayniumHarness$completed(CallbackInfoReturnable<TimerQuery.Status> callback) {
        if (rayniumHarness$pending && status == TimerQuery.Status.NOT_RECORDING && rayniumHarness$measured[currentRotationIndex])
            BenchmarkSession.referenceGpuSample(results[currentRotationIndex]);
    }
}
