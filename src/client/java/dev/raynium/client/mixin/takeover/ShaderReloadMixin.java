package dev.raynium.client.mixin.takeover;
import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import java.util.List;
import net.minecraft.client.renderer.ShaderManager;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
@Mixin(ShaderManager.class)
abstract class ShaderReloadMixin {
    @org.spongepowered.asm.mixin.injection.Inject(method="loadConfigs",at=@At("HEAD"))
    private static void raynium$validateSources(net.minecraft.server.packs.resources.ResourceManager manager,
            org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable<?> callback) {
        for(String directory:List.of("shaders","post_effect"))
            for(var entry:manager.listResources(directory,id->id.getNamespace().equals("minecraft")).entrySet())
                if(!entry.getValue().sourcePackId().equals("vanilla"))
                    throw new IllegalArgumentException("Raynium does not support resource-pack shaders or post effects: "+entry.getKey()
                        +" from "+entry.getValue().sourcePackId());
    }
    @org.spongepowered.asm.mixin.injection.Inject(method="apply",at=@At("HEAD"))
    private void raynium$postReload(org.spongepowered.asm.mixin.injection.callback.CallbackInfo ci) {
        dev.raynium.client.takeover.scene.ScenePost.reload();
    }
    @WrapOperation(method="*",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/RenderPipelines;requiredPipelines()Ljava/util/List;"))
    private static List<RenderPipeline> raynium$required(Operation<List<RenderPipeline>> original) { return List.of(); }
    @WrapOperation(method="*",at=@At(value="INVOKE",target="Lnet/minecraft/client/renderer/RenderPipelines;optionalPipelines()Ljava/util/List;"))
    private static List<RenderPipeline> raynium$optional(Operation<List<RenderPipeline>> original) { return List.of(); }
}
