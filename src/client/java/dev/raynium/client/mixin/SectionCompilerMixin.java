package dev.raynium.client.mixin;
import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.chunk.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.*;

@Mixin(SectionCompiler.class)
abstract class SectionCompilerMixin {
    @Unique private long raynium$epoch;
    @Inject(method="<init>",at=@At("RETURN"))
    private void raynium$generation(CallbackInfo ci) { raynium$epoch=TerrainCapture.epoch(); }
    @Inject(method="compile",at=@At("RETURN"))
    private void raynium$capture(CallbackInfoReturnable<SectionCompiler.Results> cir) {
        if(!TerrainCapture.active()) return;
        var result=cir.getReturnValue();
        var layers=new MeshPayload[3];
        for(var layer:ChunkSectionLayer.values()) {
            var mesh=result.renderedLayers.get(layer);
            if(mesh!=null) layers[layer.ordinal()]=new MeshPayload(raynium$epoch,mesh.vertexBuffer(),layer.ordinal());
        }
        ((PayloadOwner)(Object)result).raynium$payload(new SectionPayload(raynium$epoch,layers));
    }
}
