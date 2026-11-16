package dev.raynium.client.mixin;

import com.mojang.blaze3d.systems.TimerQuery;
import dev.raynium.client.benchmark.BenchmarkSession;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(TimerQuery.class)
abstract class BenchmarkReferenceTimerMixin {
    @Shadow private TimerQuery.Status status;
    @Shadow private int currentRotationIndex;
    @Shadow @Final private long[] results;
    @Unique private final boolean[] raynium$measured=new boolean[3];
    @Unique private boolean raynium$pending;
    @Inject(method="beginProfile",at=@At("RETURN"))
    private void raynium$started(CallbackInfo callback) { raynium$measured[currentRotationIndex]=BenchmarkSession.isMeasuring(); }
    @Inject(method="getStatus",at=@At("HEAD"))
    private void raynium$polling(CallbackInfoReturnable<TimerQuery.Status> callback) { raynium$pending=status==TimerQuery.Status.AWAITING_VALUES; }
    @Inject(method="getStatus",at=@At("RETURN"))
    private void raynium$completed(CallbackInfoReturnable<TimerQuery.Status> callback) {
        if(raynium$pending && status==TimerQuery.Status.NOT_RECORDING && raynium$measured[currentRotationIndex])
            BenchmarkSession.referenceGpuSample(results[currentRotationIndex]);
    }
}
