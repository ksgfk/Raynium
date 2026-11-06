package dev.raynium.client.mixin;

import dev.raynium.client.benchmark.BenchmarkSession;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
abstract class MinecraftBenchmarkMixin {
    @Inject(method = "renderFrame", at = @At("HEAD"))
    private void raynium$benchmarkCamera(CallbackInfo callback) {
        BenchmarkSession.beforeFrame((Minecraft) (Object) this);
    }
}
