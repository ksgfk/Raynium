package dev.raynium.client.mixin;

import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.chunk.*;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(SectionRenderDispatcher.RenderSection.class)
abstract class RenderSectionStateMixin implements SectionStateAccess {
    @Unique private volatile State raynium$state;
    @Unique private long raynium$captureGen;
    @Unique private int raynium$captureSlot=-1;

    @Override public State raynium$state() { return raynium$state; }
    @Override public long raynium$captureGen() { return raynium$captureGen; }
    @Override public int raynium$captureSlot() { return raynium$captureSlot; }
    @Override public void raynium$setCaptureSlot(long generation,int slot) {
        raynium$captureGen=generation;
        raynium$captureSlot=slot;
    }

    @Inject(method="setSectionMesh",at=@At("RETURN"))
    private void raynium$onSetSectionMesh(SectionMesh sectionMesh, CallbackInfoReturnable<SectionMesh> cir) {
        SectionPayload payload=null;
        int mask=0;
        if(sectionMesh instanceof CompiledSectionMesh && sectionMesh instanceof PayloadOwner owner) {
            payload=owner.raynium$payload();
            if(payload!=null) {
                int ready=owner.raynium$readyMask();
                var layers=payload.layers;
                if(layers[0]!=null && (ready&1)!=0) mask|=1;
                if(layers[1]!=null && (ready&(1<<8))!=0) mask|=1<<8;
                if(layers[2]!=null && (ready&(1<<16))!=0) mask|=1<<16;
            }
        }
        raynium$state=payload==null?null:new State(payload,mask);
        TerrainCapture.sectionChanged((SectionRenderDispatcher.RenderSection)(Object)this);
    }

    @Inject(method="reset",at=@At("RETURN"))
    private void raynium$onReset(CallbackInfo ci) {
        raynium$state=null;
        TerrainCapture.sectionChanged((SectionRenderDispatcher.RenderSection)(Object)this);
    }
}
