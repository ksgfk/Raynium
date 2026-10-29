package dev.raynium.client.mixin;
import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import org.spongepowered.asm.mixin.*;

@Mixin(SectionCompiler.Results.class)
abstract class SectionResultsMixin implements PayloadOwner {
    @Unique private MeshPayload raynium$payload;
    @Override public MeshPayload raynium$payload() { return raynium$payload; }
    @Override public void raynium$payload(MeshPayload payload) { raynium$payload=payload; }
}
