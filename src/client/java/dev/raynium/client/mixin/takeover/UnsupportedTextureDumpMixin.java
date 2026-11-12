package dev.raynium.client.mixin.takeover;
import com.mojang.blaze3d.platform.TextureUtil;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
@Mixin(TextureUtil.class)
abstract class UnsupportedTextureDumpMixin {
    @Unique private static boolean raynium$warned;
    @Inject(method="writeAsPNG",at=@At("HEAD"),cancellable=true)
    private static void raynium$unsupported(CallbackInfo ci) {
        if(!raynium$warned) { org.slf4j.LoggerFactory.getLogger("raynium").info("Texture PNG dumping is unavailable during Raynium takeover"); raynium$warned=true; }
        ci.cancel();
    }
}
