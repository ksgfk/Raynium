package dev.raynium.client.mixin;
import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.chunk.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(CompiledSectionMesh.class)
abstract class CompiledSectionMeshMixin implements PayloadOwner {
    @Unique private MeshPayload raynium$payload;
    @Override public MeshPayload raynium$payload() { return raynium$payload; }
    @Override public void raynium$payload(MeshPayload payload) { raynium$payload=payload; }
    @Inject(method="<init>",at=@At("RETURN"))
    private void raynium$attach(TranslucencyPointOfView pov, SectionCompiler.Results results, long started, CallbackInfo ci) {
        raynium$payload=((PayloadOwner)(Object)results).raynium$payload();
        ((PayloadOwner)(Object)results).raynium$payload(null);
    }
    @Inject(method="close",at=@At("HEAD"))
    private void raynium$release(CallbackInfo ci) { TerrainCapture.removed(raynium$payload); raynium$payload=null; }
}
