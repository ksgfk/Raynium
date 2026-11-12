package dev.raynium.client.mixin.takeover;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.renderpearl.api.device.GpuSurface;
import dev.raynium.client.takeover.TakeoverRuntime;
import java.util.Optional;
import net.minecraft.client.gui.components.debug.DebugEntryFps;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
@Mixin(DebugEntryFps.class)
abstract class DebugSurfaceMixin {
    @WrapOperation(method="display",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuSurface;currentConfiguration()Ljava/util/Optional;"))
    private Optional<GpuSurface.Configuration> raynium$config(GpuSurface surface,Operation<Optional<GpuSurface.Configuration>> original) { return TakeoverRuntime.configuration(); }
}
