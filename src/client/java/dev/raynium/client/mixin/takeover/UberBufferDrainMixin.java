package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.vertex.*;
import dev.raynium.client.takeover.TerrainDrain;
import it.unimi.dsi.fastutil.objects.*;
import org.spongepowered.asm.mixin.*;

@Mixin(UberGpuBuffer.class)
abstract class UberBufferDrainMixin<T> implements TerrainDrain {
    @Shadow @Final private StagingBuffer stagingBuffer;
    @Shadow @Final private Object2ObjectOpenHashMap<T,UberGpuBuffer.StagedAllocationEntry<? extends T>> stagedAllocations;
    @Shadow @Final private ObjectOpenHashSet<T> skippedStagedAllocations;
    @Shadow private static <T,U extends T> void runCallbackUnchecked(T key,UberGpuBuffer.StagedAllocationEntry<U> entry) { throw new AssertionError(); }
    @Override public void raynium$drain(StagingBuffer.Uploader uploader) {
        uploader.checkValidFor(stagingBuffer);
        try {
            for(var entry:stagedAllocations.entrySet()) try(var staged=entry.getValue()) {
                if(!skippedStagedAllocations.contains(entry.getKey())) runCallbackUnchecked(entry.getKey(),staged);
            }
        } finally {
            stagedAllocations.values().forEach(UberGpuBuffer.StagedAllocationEntry::close);
            stagedAllocations.clear(); skippedStagedAllocations.clear();
        }
    }
}
