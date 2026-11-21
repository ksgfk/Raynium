package dev.raynium.client.world;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.renderer.StagedVertexBuffer;
import net.minecraft.client.renderer.oit.OitPipelineSet;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.rendertype.PreparedRenderType;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.joml.Vector3f;
import org.jspecify.annotations.Nullable;

/** Coordinates world rigid ModelPart capture, meshId run grouping, OIT upload reuse, and fallback draws. */
public final class ModelFastTrack {
    public interface FeatureRendererAccess {
        GroupAccess raynium$currentGroupAccess();
    }

    public interface GroupAccess {
        GroupExtension raynium$extension();
        @Nullable GroupExtension raynium$extensionOrNull();
        void raynium$recycleExtension();
        PreparedRenderType raynium$prepareRenderType(RenderType renderType);
        VertexConsumer raynium$fallbackConsumer(RenderType renderType);
        List<StagedVertexBuffer.Draw> raynium$draws();
        List<PreparedRenderType> raynium$drawRenderTypes();
    }

    private static final int MAX_POOLED_GROUPS = 64;
    private static final int MAX_POOLED_BUCKETS = 256;
    private static final ArrayDeque<GroupExtension> GROUP_POOL = new ArrayDeque<>(32);
    private static final ArrayDeque<FastBucket> BUCKET_POOL = new ArrayDeque<>(64);
    private static final MeshRunTable RUN_TABLE = new MeshRunTable();

    private static @Nullable ModelSink activeSink;

    private ModelFastTrack() {}

    public static void beginCapture(ModelSink sink) { activeSink = sink; }
    public static void endCapture() { activeSink = null; }

    public static GroupExtension acquireGroupExtension(boolean canReorder, int existingFallbackCount) {
        GroupExtension ext = GROUP_POOL.pollFirst();
        if (ext == null) {
            ext = new GroupExtension();
        }
        ext.init(canReorder, existingFallbackCount);
        return ext;
    }

    static void recycleGroupExtension(GroupExtension ext) {
        if (GROUP_POOL.size() < MAX_POOLED_GROUPS) {
            GROUP_POOL.addFirst(ext);
        }
    }

    static FastBucket acquireFastBucket(PreparedRenderType renderType, boolean canReorder) {
        FastBucket bucket = BUCKET_POOL.pollFirst();
        if (bucket == null) {
            bucket = new FastBucket();
        }
        bucket.init(renderType, canReorder);
        return bucket;
    }

    static void recycleFastBucket(FastBucket bucket) {
        bucket.reset();
        if (BUCKET_POOL.size() < MAX_POOLED_BUCKETS) {
            BUCKET_POOL.addFirst(bucket);
        }
    }

    /** Render-thread reusable open-addressing int->int table with O(1) generation-stamped reset. */
    static final class MeshRunTable {
        private int[] keys = new int[64];
        private int[] values = new int[64];
        private int[] stamps = new int[64];
        private int mask = 63;
        private int size;
        private int stamp = 1;

        void reset() {
            size = 0;
            if (++stamp == 0) {
                Arrays.fill(stamps, 0);
                stamp = 1;
            }
        }

        int getOrInsert(int key, int newRun) {
            if ((size + 1) * 2 > keys.length) {
                grow();
            }
            int slot = mix(key) & mask;
            while (stamps[slot] == stamp) {
                if (keys[slot] == key) return values[slot];
                slot = (slot + 1) & mask;
            }
            stamps[slot] = stamp;
            keys[slot] = key;
            values[slot] = newRun;
            ++size;
            return -1;
        }

        private void grow() {
            int oldCap = keys.length;
            int newCap = oldCap * 2;
            int newMask = newCap - 1;
            int[] newKeys = new int[newCap];
            int[] newValues = new int[newCap];
            int[] newStamps = new int[newCap];
            int currentStamp = stamp;
            for (int i = 0; i < oldCap; ++i) {
                if (stamps[i] != currentStamp) continue;
                int k = keys[i];
                int slot = mix(k) & newMask;
                while (newStamps[slot] == currentStamp) {
                    slot = (slot + 1) & newMask;
                }
                newStamps[slot] = currentStamp;
                newKeys[slot] = k;
                newValues[slot] = values[i];
            }
            keys = newKeys;
            values = newValues;
            stamps = newStamps;
            mask = newMask;
        }

        private static int mix(int x) {
            int h = x * 0x9E3779B9;
            return h ^ (h >>> 16);
        }
    }

    public static boolean supportsOitInstanced(RayniumGpuDevice device, OitPipelineSet oitSet) {
        return device.supportsInstanced(oitSet.getPipeline(OitStage.DEPTH_BOUNDS), "entity")
            && device.supportsInstanced(oitSet.getPipeline(OitStage.TRANSMITTANCE), "entity")
            && device.supportsInstanced(oitSet.getPipeline(OitStage.ACCUMULATE), "entity");
    }

    public static boolean tryCompilePart(ModelPart part, PoseStack.Pose pose, VertexConsumer builder,
            int lightCoords, int overlayCoords, int color) {
        ModelSink sink = activeSink;
        if (sink == null || builder != sink) return false;
        int meshId = RetainedModelManager.resolveMeshId(sink.device, part);
        if (meshId == 0) return true; // Empty ModelPart: nothing to draw.
        if (meshId < 0) return false; // Ineligible geometry: fall back to vanilla cube.compile into sink.
        sink.recordInstance(part, meshId, ((RetainedModelManager.PartHandle)(Object)part).raynium$getMeshPage(), pose, color, overlayCoords, lightCoords);
        return true;
    }

    public static boolean tryCompileTranslatedLeaf(ModelPart part, PoseStack.Pose pose, VertexConsumer builder,
            float tx, float ty, float tz, int lightCoords, int overlayCoords, int color) {
        ModelSink sink = activeSink;
        if (sink == null || builder != sink) return false;
        if (!RetainedModelManager.hasResidentMesh(sink.device,(RetainedModelManager.PartHandle)(Object)part)) return false;
        int meshId = RetainedModelManager.resolveMeshId(sink.device, part);
        if (meshId == 0) return true;
        if (meshId < 0) return false;
        sink.recordTranslatedInstance(part, meshId, ((RetainedModelManager.PartHandle)(Object)part).raynium$getMeshPage(), pose, tx, ty, tz, color, overlayCoords, lightCoords);
        return true;
    }

    public static final class GroupExtension {
        private boolean canReorder;
        private final List<Object> entries = new ArrayList<>();
        private int syncedFallbackCount;
        private @Nullable FastBucket lastFastBucket;
        private @Nullable RenderType lastFastRenderType;
        private RenderType[] idKeys = new RenderType[16];
        private FastBucket[] idBuckets = new FastBucket[16];
        private int[] idStamps = new int[16];
        private int[] valueEntries = new int[16];
        private int[] valueStamps = new int[16];
        private int valueSize;
        private int idMask = 15;
        private int idSize;
        private int idStamp = 1;
        private final Vector3f scratchNormal = new Vector3f();

        GroupExtension() {}

        void init(boolean canReorder, int existingFallbackCount) {
            this.canReorder = canReorder;
            this.entries.clear();
            for (int i = 0; i < existingFallbackCount; ++i) {
                entries.add(Integer.valueOf(i));
            }
            this.syncedFallbackCount = existingFallbackCount;
            this.lastFastBucket = null;
            this.lastFastRenderType = null;
            this.valueSize = 0;
            this.idSize = 0;
            if (++this.idStamp == 0) {
                Arrays.fill(valueStamps, 0);
                Arrays.fill(idStamps, 0);
                this.idStamp = 1;
            }
        }

        public void recycle() {
            for (int i = 0; i < entries.size(); ++i) {
                if (entries.get(i) instanceof FastBucket bucket) {
                    recycleFastBucket(bucket);
                }
            }
            entries.clear();
            if (idSize > 0) {
                Arrays.fill(idKeys, null);
                Arrays.fill(idBuckets, null);
                idSize = 0;
            }
            syncedFallbackCount = 0;
            lastFastBucket = null;
            lastFastRenderType = null;
            recycleGroupExtension(this);
        }

        private static int idHash(RenderType key) {
            int h = System.identityHashCode(key) * 0x9E3779B9;
            return h ^ (h >>> 16);
        }

        private @Nullable FastBucket lookupByIdentity(RenderType key) {
            if (idSize == 0) return null;
            int slot = idHash(key) & idMask;
            int stamp = idStamp;
            while (idStamps[slot] == stamp) {
                if (idKeys[slot] == key) return idBuckets[slot];
                slot = (slot + 1) & idMask;
            }
            return null;
        }

        private void putByIdentity(RenderType key, FastBucket bucket) {
            if ((idSize + 1) * 2 > idKeys.length) {
                growIdentityTable();
            }
            int slot = idHash(key) & idMask;
            int stamp = idStamp;
            while (idStamps[slot] == stamp) {
                if (idKeys[slot] == key) {
                    idBuckets[slot] = bucket;
                    return;
                }
                slot = (slot + 1) & idMask;
            }
            idStamps[slot] = stamp;
            idKeys[slot] = key;
            idBuckets[slot] = bucket;
            ++idSize;
        }

        private void growIdentityTable() {
            int oldCap = idKeys.length;
            int newCap = oldCap * 2;
            int newMask = newCap - 1;
            RenderType[] newKeys = new RenderType[newCap];
            FastBucket[] newBuckets = new FastBucket[newCap];
            int[] newStamps = new int[newCap];
            int stamp = idStamp;
            for (int i = 0; i < oldCap; ++i) {
                if (idStamps[i] != stamp) continue;
                RenderType k = idKeys[i];
                int slot = idHash(k) & newMask;
                while (newStamps[slot] == stamp) {
                    slot = (slot + 1) & newMask;
                }
                newStamps[slot] = stamp;
                newKeys[slot] = k;
                newBuckets[slot] = idBuckets[i];
            }
            idKeys = newKeys;
            idBuckets = newBuckets;
            idStamps = newStamps;
            idMask = newMask;
        }

        private int valueSlot(PreparedRenderType key) {
            int mask = valueEntries.length - 1;
            int slot = MeshRunTable.mix(key.hashCode()) & mask;
            while (valueStamps[slot] == idStamp) {
                FastBucket existing = (FastBucket) entries.get(valueEntries[slot]);
                if (existing.renderType.equals(key)) break;
                slot = (slot + 1) & mask;
            }
            return slot;
        }

        private void indexValue(PreparedRenderType key, int index) {
            if ((valueSize + 1) * 2 > valueEntries.length) {
                int[] old = valueEntries, stamps = valueStamps;
                valueEntries = new int[old.length * 2];
                valueStamps = new int[valueEntries.length];
                for (int i = 0; i < old.length; i++) {
                    if (stamps[i] != idStamp) continue;
                    int slot = valueSlot(((FastBucket) entries.get(old[i])).renderType);
                    valueEntries[slot] = old[i];
                    valueStamps[slot] = idStamp;
                }
            }
            int slot = valueSlot(key);
            valueEntries[slot] = index;
            valueStamps[slot] = idStamp;
            ++valueSize;
        }

        public void syncFallbackDraws(int totalFallbackCount) {
            while (syncedFallbackCount < totalFallbackCount) {
                entries.add(Integer.valueOf(syncedFallbackCount++));
                if (!canReorder) {
                    lastFastBucket = null;
                    lastFastRenderType = null;
                }
            }
        }

        FastBucket acquireFastBucket(RenderType renderType, PreparedRenderType prepared) {
            boolean consolidate = canReorder && renderType.canConsolidateConsecutiveGeometry();
            if (lastFastBucket != null && lastFastRenderType == renderType && renderType.canConsolidateConsecutiveGeometry()) {
                return lastFastBucket;
            }
            if (consolidate) {
                FastBucket byId = lookupByIdentity(renderType);
                if (byId != null) {
                    lastFastBucket = byId;
                    lastFastRenderType = renderType;
                    return byId;
                }
                int slot = valueSlot(prepared);
                if (valueStamps[slot] == idStamp) {
                    lastFastBucket = (FastBucket) entries.get(valueEntries[slot]);
                    lastFastRenderType = renderType;
                    return lastFastBucket;
                }
            }
            FastBucket bucket = ModelFastTrack.acquireFastBucket(prepared, consolidate);
            entries.add(bucket);
            if (consolidate) {
                putByIdentity(renderType, bucket);
                indexValue(prepared, entries.size() - 1);
            }
            lastFastBucket = bucket;
            lastFastRenderType = renderType;
            return bucket;
        }

        public void addShadow(ShadowFastTrack shadow) {
            entries.add(shadow);
        }

        Vector3f scratchNormal() { return scratchNormal; }

        public void execute(StagedVertexBuffer stagedBuffer, @Nullable OitStage stage, RenderPass renderPass,
                List<StagedVertexBuffer.Draw> draws, List<PreparedRenderType> drawRenderTypes) {
            for (int i = 0; i < entries.size(); ++i) {
                Object entry = entries.get(i);
                if (entry instanceof FastBucket bucket) {
                    bucket.execute(stage, renderPass);
                } else if (entry instanceof ShadowFastTrack shadow) {
                    shadow.execute(stage, renderPass);
                } else {
                    int idx = ((Integer) entry).intValue();
                    PreparedRenderType renderType = drawRenderTypes.get(idx);
                    StagedVertexBuffer.ExecuteInfo info = stagedBuffer.getExecuteInfo(draws.get(idx));
                    if (info != null) {
                        if (stage != null) renderType.drawFromBufferOit(info, stage, renderPass);
                        else renderType.drawFromBuffer(info, renderPass);
                    }
                }
            }
        }
    }

    static RenderPipeline bind(PreparedRenderType renderType, @Nullable OitStage stage, RenderPass renderPass) {
        RenderPipeline targetPipeline = stage != null
            ? Objects.requireNonNull(renderType.oitPipelineSet(), "Render type " + renderType.name() + " missing OIT pipeline set").getPipeline(stage)
            : renderType.pipeline();
        renderPass.setPipeline(RenderSystem.getCompiledPipeline(targetPipeline));
        if (renderType.scissorState().enabled()) {
            var scissor = renderType.scissorState();
            renderPass.enableScissor(scissor.x(), scissor.y(), scissor.width(), scissor.height());
        }
        RenderSystem.bindDefaultUniforms(renderPass);
        renderPass.setUniform("DynamicTransforms", renderType.dynamicTransforms());
        for (PreparedRenderType.Texture texture : renderType.textures()) {
            renderPass.setUniform(texture.name(), texture.textureView(), texture.sampler());
        }
        return targetPipeline;
    }

    static final class FastBucket {
        PreparedRenderType renderType;
        boolean canReorder;
        private ModelPart[] owners = new ModelPart[32];
        private int[] pageIds = new int[32];
        private int[] meshIds = new int[32];
        private int[] instanceRun = new int[32];
        private int[] instanceInts = new int[32 * RetainedModelManager.INSTANCE_INTS];
        private int instanceCount;
        private @Nullable CommandTape uploadedTape;
        private long uploadedOffset;
        private int[] runMeshIds = new int[8];
        private int[] runFirstInstances = new int[8];
        private int[] runInstanceCounts = new int[8];
        private final MeshRunTable pageTable = new MeshRunTable();
        private int[] runPages = new int[8], pageCursors = new int[8];
        private int[] groupedIds = new int[8], groupedCounts = new int[8];
        private int[] ownerIndices = new int[8];
        private int[] runCursors = new int[8];
        private int runCount;

        FastBucket() {}

        void init(PreparedRenderType renderType, boolean canReorder) {
            this.renderType = renderType;
            this.canReorder = canReorder;
            this.instanceCount = 0;
            this.uploadedTape = null;
            this.uploadedOffset = 0;
            this.runCount = 0;
        }

        void reset() {
            Arrays.fill(owners, 0, instanceCount, null);
            this.renderType = null;
            this.uploadedTape = null;
            this.uploadedOffset = 0;
            this.instanceCount = 0;
            this.runCount = 0;
        }

        void addInstance(ModelPart owner, int meshId, int pageId, PoseStack.Pose pose, int colorArgb, int overlayCoords, int lightCoords, Vector3f scratch) {
            if (instanceCount == meshIds.length) {
                int next = meshIds.length * 2;
                owners = Arrays.copyOf(owners, next);
                pageIds = Arrays.copyOf(pageIds, next);
                meshIds = Arrays.copyOf(meshIds, next);
                instanceRun = Arrays.copyOf(instanceRun, next);
                instanceInts = Arrays.copyOf(instanceInts, next * RetainedModelManager.INSTANCE_INTS);
            }
            owners[instanceCount] = owner;
            pageIds[instanceCount] = pageId;
            meshIds[instanceCount] = meshId;
            RetainedModelManager.packInstance(pose, colorArgb, overlayCoords, lightCoords,
                instanceInts, instanceCount * RetainedModelManager.INSTANCE_INTS, scratch);
            ++instanceCount;
        }

        void addTranslatedInstance(ModelPart owner, int meshId, int pageId, PoseStack.Pose pose, float tx, float ty, float tz,
                int colorArgb, int overlayCoords, int lightCoords, Vector3f scratch) {
            if (instanceCount == meshIds.length) {
                int next = meshIds.length * 2;
                owners = Arrays.copyOf(owners, next);
                pageIds = Arrays.copyOf(pageIds, next);
                meshIds = Arrays.copyOf(meshIds, next);
                instanceRun = Arrays.copyOf(instanceRun, next);
                instanceInts = Arrays.copyOf(instanceInts, next * RetainedModelManager.INSTANCE_INTS);
            }
            owners[instanceCount] = owner;
            pageIds[instanceCount] = pageId;
            meshIds[instanceCount] = meshId;
            RetainedModelManager.packInstanceTranslated(pose, tx, ty, tz, colorArgb, overlayCoords, lightCoords,
                instanceInts, instanceCount * RetainedModelManager.INSTANCE_INTS, scratch);
            ++instanceCount;
        }

        private void ensureUploaded(CommandTape tape) {
            if (uploadedTape == tape) return;
            long totalBytes = (long) instanceCount * RetainedModelManager.INSTANCE_BYTES;
            long offset = tape.reserveInstanceUpload(totalBytes, 8);
            MemorySegment dst = tape.instanceUploadSlice(offset, totalBytes);
            if (!canReorder || instanceCount == 1) {
                MemorySegment.copy(instanceInts, 0, dst, ValueLayout.JAVA_INT, 0, instanceCount * RetainedModelManager.INSTANCE_INTS);
                buildSequentialRuns();
            } else {
                buildGroupedAndCopy(dst);
            }
            for (int r = 0; r < runCount; ++r) tape.retainModel(owners[ownerIndices[r]]);
            uploadedTape = tape;
            uploadedOffset = offset;
        }

        private void buildSequentialRuns() {
            int runs = 0;
            int prev = -1;
            for (int i = 0; i < instanceCount; ++i) {
                if (meshIds[i] != prev) { ++runs; prev = meshIds[i]; }
            }
            ensureRunCapacity(runs);
            runCount = 0;
            int start = 0;
            while (start < instanceCount) {
                int id = meshIds[start];
                int end = start + 1;
                while (end < instanceCount && meshIds[end] == id) ++end;
                ownerIndices[runCount] = start;
                runMeshIds[runCount] = id;
                runFirstInstances[runCount] = start;
                runInstanceCounts[runCount] = end - start;
                ++runCount;
                start = end;
            }
        }

        void buildGroupedAndCopy(MemorySegment dst) {
            // Fast check: if all instances share a single meshId, copy directly without hashing.
            int firstId = meshIds[0];
            boolean singleMesh = true;
            for (int i = 1; i < instanceCount; ++i) {
                if (meshIds[i] != firstId) { singleMesh = false; break; }
            }
            if (singleMesh) {
                MemorySegment.copy(instanceInts, 0, dst, ValueLayout.JAVA_INT, 0, instanceCount * RetainedModelManager.INSTANCE_INTS);
                ensureRunCapacity(1);
                ownerIndices[0] = 0;
                runMeshIds[0] = firstId;
                runFirstInstances[0] = 0;
                runInstanceCounts[0] = instanceCount;
                runCount = 1;
                return;
            }
            RUN_TABLE.reset();
            int runs = 0, pages = 0;
            pageTable.reset();
            for (int i = 0; i < instanceCount; ++i) {
                int id = meshIds[i];
                int r = RUN_TABLE.getOrInsert(id, runs);
                if (r < 0) {
                    r = runs++;
                    ensureRunCapacity(runs);
                    int page = pageTable.getOrInsert(pageIds[i], pages);
                    if (page < 0) {
                        page = pages++;
                        pageCursors[page] = 0;
                    }
                    runPages[r] = page;
                    ++pageCursors[page];
                    ownerIndices[r] = i;
                    runMeshIds[r] = id;
                    runInstanceCounts[r] = 1;
                } else {
                    ++runInstanceCounts[r];
                }
                instanceRun[i] = r;
            }
            if (pages > 1) {
                int start = 0;
                for (int p = 0; p < pages; ++p) {
                    int count = pageCursors[p];
                    pageCursors[p] = start;
                    start += count;
                }
                for (int r = 0; r < runs; ++r) {
                    int grouped = pageCursors[runPages[r]]++;
                    groupedIds[grouped] = runMeshIds[r];
                    groupedCounts[grouped] = runInstanceCounts[r];
                    runPages[r] = grouped;
                }
                System.arraycopy(groupedIds, 0, runMeshIds, 0, runs);
                System.arraycopy(groupedCounts, 0, runInstanceCounts, 0, runs);
                for (int i = 0; i < instanceCount; ++i) instanceRun[i] = runPages[instanceRun[i]];
            }
            runCount = runs;
            int prefix = 0;
            for (int r = 0; r < runs; ++r) {
                runFirstInstances[r] = prefix;
                runCursors[r] = prefix;
                prefix += runInstanceCounts[r];
            }
            for (int i = 0; i < instanceCount; ++i) {
                int dstIndex = runCursors[instanceRun[i]]++;
                MemorySegment.copy(instanceInts, i * RetainedModelManager.INSTANCE_INTS,
                    dst, ValueLayout.JAVA_INT, (long) dstIndex * RetainedModelManager.INSTANCE_BYTES,
                    RetainedModelManager.INSTANCE_INTS);
            }
        }

        private void ensureRunCapacity(int needed) {
            if (runMeshIds.length < needed) {
                int cap = Math.max(needed, Math.max(8, runMeshIds.length * 2));
                runMeshIds = Arrays.copyOf(runMeshIds, cap);
                runFirstInstances = Arrays.copyOf(runFirstInstances, cap);
                runInstanceCounts = Arrays.copyOf(runInstanceCounts, cap);
                ownerIndices = Arrays.copyOf(ownerIndices, cap);
                runPages = Arrays.copyOf(runPages, cap);
                pageCursors = Arrays.copyOf(pageCursors, cap);
                groupedIds = Arrays.copyOf(groupedIds, cap);
                groupedCounts = Arrays.copyOf(groupedCounts, cap);
                runCursors = Arrays.copyOf(runCursors, cap);
            }
        }

        int runCount() { return runCount; }
        int runMeshId(int r) { return runMeshIds[r]; }
        int runFirstInstance(int r) { return runFirstInstances[r]; }
        int runInstanceCount(int r) { return runInstanceCounts[r]; }

        void execute(@Nullable OitStage stage, RenderPass renderPass) {
            if (instanceCount == 0) return;
            CommandTape tape = RayniumGpuDevice.activeTape(renderPass);
            ensureUploaded(tape);
            renderPass.pushDebugGroup(() -> "Render Type " + renderType.name());
            var targetPipeline = bind(renderType, stage, renderPass);
            if (!RayniumGpuDevice.recordInstancedGeometry(renderPass, "entity", uploadedOffset, instanceCount, RetainedModelManager.INSTANCE_BYTES,
                    runMeshIds, runFirstInstances, runInstanceCounts, runCount)) {
                throw new IllegalStateException("Instanced rigid model pipeline unavailable during execute for " + targetPipeline.getLocation());
            }
            renderPass.popDebugGroup();
        }
    }

    /** Lazy fallback VertexConsumer: avoids allocating a StagedVertexBuffer.Draw unless fallback vertices are emitted. */
    public static final class ModelSink implements VertexConsumer {
        RayniumGpuDevice device;
        private GroupAccess group;
        private RenderType renderType;
        private PreparedRenderType prepared;
        private @Nullable FastBucket fastBucket;
        private @Nullable VertexConsumer fallback;

        public void begin(RayniumGpuDevice device, GroupAccess group, RenderType renderType, PreparedRenderType prepared) {
            this.device = device;
            this.group = group;
            this.renderType = renderType;
            this.prepared = prepared;
            this.fastBucket = null;
            this.fallback = null;
        }

        public void end() {
            this.device = null;
            this.group = null;
            this.renderType = null;
            this.prepared = null;
            this.fastBucket = null;
            this.fallback = null;
        }

        void recordInstance(ModelPart owner, int meshId, int pageId, PoseStack.Pose pose, int colorArgb, int overlayCoords, int lightCoords) {
            GroupExtension ext = group.raynium$extension();
            if (fastBucket == null) {
                fastBucket = ext.acquireFastBucket(renderType, prepared);
            }
            fastBucket.addInstance(owner, meshId, pageId, pose, colorArgb, overlayCoords, lightCoords, ext.scratchNormal());
        }

        void recordTranslatedInstance(ModelPart owner, int meshId, int pageId, PoseStack.Pose pose, float tx, float ty, float tz,
                int colorArgb, int overlayCoords, int lightCoords) {
            GroupExtension ext = group.raynium$extension();
            if (fastBucket == null) {
                fastBucket = ext.acquireFastBucket(renderType, prepared);
            }
            fastBucket.addTranslatedInstance(owner, meshId, pageId, pose, tx, ty, tz, colorArgb, overlayCoords, lightCoords, ext.scratchNormal());
        }

        private VertexConsumer fallback() {
            if (fallback == null) {
                fallback = group.raynium$fallbackConsumer(renderType);
                fastBucket = null; // In strictlyOrdered mode, subsequent fast parts after fallback start a new bucket.
            }
            return fallback;
        }

        @Override public VertexConsumer addVertex(float x, float y, float z) { return fallback().addVertex(x, y, z); }
        @Override public VertexConsumer setColor(int r, int g, int b, int a) { return fallback().setColor(r, g, b, a); }
        @Override public VertexConsumer setColor(int color) { return fallback().setColor(color); }
        @Override public VertexConsumer setUv(float u, float v) { return fallback().setUv(u, v); }
        @Override public VertexConsumer setUv1(int u, int v) { return fallback().setUv1(u, v); }
        @Override public VertexConsumer setUv2(int u, int v) { return fallback().setUv2(u, v); }
        @Override public VertexConsumer setUv3(float u, float v) { return fallback().setUv3(u, v); }
        @Override public VertexConsumer setNormal(float x, float y, float z) { return fallback().setNormal(x, y, z); }
        @Override public VertexConsumer setLineWidth(float width) { return fallback().setLineWidth(width); }
        @Override public void addVertex(float x, float y, float z, int color, float u, float v, int overlayCoords, int lightCoords, float nx, float ny, float nz) {
            fallback().addVertex(x, y, z, color, u, v, overlayCoords, lightCoords, nx, ny, nz);
        }
    }
}
