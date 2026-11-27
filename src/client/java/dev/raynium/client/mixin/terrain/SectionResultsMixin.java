package dev.raynium.client.mixin.terrain;
import dev.raynium.client.terrain.PayloadOwner;
import dev.raynium.client.terrain.SectionPayload;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;

@Mixin(SectionCompiler.Results.class)
abstract class SectionResultsMixin implements PayloadOwner {
    @Unique
    private SectionPayload raynium$payload;
    @Override
    public SectionPayload raynium$payload() {
        return raynium$payload;
    }
    @Override
    public void raynium$payload(SectionPayload payload) {
        raynium$payload = payload;
    }
}
