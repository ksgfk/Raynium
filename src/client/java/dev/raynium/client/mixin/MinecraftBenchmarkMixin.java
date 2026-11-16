package dev.raynium.client.mixin;

import dev.raynium.client.benchmark.BenchmarkSession;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;

@Mixin(Minecraft.class)
abstract class MinecraftBenchmarkMixin {
    @WrapOperation(method="renderFrame",at=@At(value="INVOKE",target="Lnet/minecraft/client/gui/components/debug/DebugScreenEntryList;isCurrentlyEnabled(Lnet/minecraft/resources/Identifier;)Z"))
    private boolean raynium$referenceTiming(net.minecraft.client.gui.components.debug.DebugScreenEntryList entries,net.minecraft.resources.Identifier id,Operation<Boolean> original) {
        return (dev.raynium.client.RenderMode.REFERENCE && BenchmarkSession.isActive()) || original.call(entries,id);
    }
    @Inject(method = "renderFrame", at = @At("HEAD"))
    private void raynium$benchmarkCamera(CallbackInfo callback) {
        BenchmarkSession.beforeFrame((Minecraft) (Object) this);
        dev.raynium.client.validation.VisualValidation.beforeFrame((Minecraft)(Object)this);
    }
    @Inject(method = "renderFrame", at = @At("RETURN"))
    private void raynium$referenceFrame(CallbackInfo callback) {
        if(dev.raynium.client.RenderMode.REFERENCE && ((Minecraft)(Object)this).level!=null) BenchmarkSession.nativeFrame();
        BenchmarkSession.afterFrame();
        dev.raynium.client.validation.VisualValidation.afterFrame((Minecraft)(Object)this);
    }
}
