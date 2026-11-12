package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.mojang.blaze3d.platform.Window;
import com.mojang.renderpearl.api.device.GpuBackend;
import org.lwjgl.sdl.SDLVideo;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(Window.class)
abstract class WindowMixin {
    @WrapOperation(method="createWindow",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuBackend;createWindow(Ljava/lang/String;IIJ)J"))
    private long raynium$window(GpuBackend backend,String title,int width,int height,long flags,Operation<Long> original) {
        return SDLVideo.SDL_CreateWindow(title,width,height,flags);
    }
}
