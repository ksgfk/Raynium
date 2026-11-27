package dev.raynium.client.mixin;

import com.mojang.blaze3d.vertex.*;
import dev.raynium.client.terrain.TerrainDrain;
import it.unimi.dsi.fastutil.objects.*;
import org.spongepowered.asm.mixin.*;

@Mixin(UberGpuBuffer.class)
abstract class UberBufferDrainMixin<T> implements TerrainDrain {
    @Shadow @Final private StagingBuffer stagingBuffer;
    @Shadow @Final private Object2ObjectOpenHashMap<T,UberGpuBuffer.StagedAllocationEntry<? extends T>> stagedAllocations;
    @Shadow @Final private ObjectOpenHashSet<T> skippedStagedAllocations;
    @Shadow private static <T,U extends T> void runCallbackUnchecked(T key,UberGpuBuffer.StagedAllocationEntry<U> entry) { throw new AssertionError(); }
    @Unique private boolean raynium$largeTables;
    @Unique private int raynium$quietDrains;

    @Unique private void raynium$clearTables(int entries) {
        stagedAllocations.clear();
        skippedStagedAllocations.clear();
        if(entries>=4096) raynium$largeTables=true;
        if(!raynium$largeTables) return;
        if(entries>256) raynium$quietDrains=0;
        else if(++raynium$quietDrains>=120) {
            raynium$largeTables=!(stagedAllocations.trim() && skippedStagedAllocations.trim());
            raynium$quietDrains=0;
        }
    }
    @Override public void raynium$drain(StagingBuffer.Uploader uploader) {
        uploader.checkValidFor(stagingBuffer);
        int entries=Math.max(stagedAllocations.size(),skippedStagedAllocations.size());
        if(stagedAllocations.isEmpty()) {
            raynium$clearTables(entries);
            return;
        }
        try {
            for(var entry:stagedAllocations.entrySet()) try(var staged=entry.getValue()) {
                if(!skippedStagedAllocations.contains(entry.getKey())) runCallbackUnchecked(entry.getKey(),staged);
            }
        } finally {
            stagedAllocations.values().forEach(UberGpuBuffer.StagedAllocationEntry::close);
            raynium$clearTables(entries);
        }
    }
}
