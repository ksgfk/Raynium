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
import java.util.HashMap;
import java.util.IdentityHashMap;
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
        @Nullable
        GroupExtension raynium$extensionOrNull();
        void raynium$recycleExtension();
        PreparedRenderType raynium$prepareRenderType(RenderType renderType);
        VertexConsumer raynium$fallbackConsumer(RenderType renderType);
        List<StagedVertexBuffer.Draw> raynium$draws();
        List<PreparedRenderType> raynium$drawRenderTypes();
    }

    private static final int MAX_CACHED_INSTANCES_PER_BUCKET = 256;
    private static final int MAX_POOLED_BUCKETS = 256;
    private static final ArrayDeque<FastBucket> BUCKET_POOL = new ArrayDeque<>(64);
    private static GroupingScratch grouping = new GroupingScratch();

    private static @Nullable ModelSink activeSink;

    private ModelFastTrack() {}

    public static void beginCapture(ModelSink sink) {
        activeSink = sink;
    }
    public static void endCapture() {
        activeSink = null;
    }

    public static GroupExtension acquireGroupExtension(boolean canReorder, int existingFallbackCount) {
        return new GroupExtension(canReorder, existingFallbackCount);
    }

    public static void finishGrouping() {
        if (grouping.instanceRun.length > 4096 && grouping.peakInstances <= 256) {
            if (++grouping.quietExecutions >= 120)
                grouping = new GroupingScratch();
        } else
            grouping.quietExecutions = 0;
        grouping.peakInstances = 0;
    }
    static void clearCachedGeometry() {
        grouping = new GroupingScratch();
        BUCKET_POOL.clear();
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
        if (bucket.meshIds.length <= MAX_CACHED_INSTANCES_PER_BUCKET && BUCKET_POOL.size() < MAX_POOLED_BUCKETS) {
            bucket.reset();
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
            int slot = mix(key) & mask;
            while (stamps[slot] == stamp) {
                if (keys[slot] == key)
                    return values[slot];
                slot = (slot + 1) & mask;
            }
            if (size == keys.length / 2) {
                grow();
                return getOrInsert(key, newRun);
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
                if (stamps[i] != currentStamp)
                    continue;
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
        return device.supportsInstanced(oitSet.getPipeline(OitStage.DEPTH_BOUNDS), "entity") && device.supportsInstanced(oitSet.getPipeline(OitStage.TRANSMITTANCE), "entity") && device.supportsInstanced(oitSet.getPipeline(OitStage.ACCUMULATE), "entity");
    }

    public static boolean tryCompilePart(ModelPart part, PoseStack.Pose pose, VertexConsumer builder,
                                         int lightCoords, int overlayCoords, int color) {
        ModelSink sink = activeSink;
        if (sink == null || builder != sink)
            return false;
        int meshId = RetainedModelManager.resolveMeshId(sink.device, part);
        if (meshId == 0)
            return true; // Empty ModelPart: nothing to draw.
        if (meshId < 0)
            return false; // Ineligible geometry: fall back to vanilla cube.compile into sink.
        sink.recordInstance(part, meshId, ((RetainedModelManager.PartHandle)(Object)part).raynium$getMeshPage(), pose, color, overlayCoords, lightCoords);
        return true;
    }

    public static boolean tryCompileTranslatedLeaf(ModelPart part, PoseStack.Pose pose, VertexConsumer builder,
                                                   float tx, float ty, float tz, int lightCoords, int overlayCoords, int color) {
        ModelSink sink = activeSink;
        if (sink == null || builder != sink)
            return false;
        if (!RetainedModelManager.hasResidentMesh(sink.device, (RetainedModelManager.PartHandle)(Object)part))
            return false;
        int meshId = RetainedModelManager.resolveMeshId(sink.device, part);
        if (meshId == 0)
            return true;
        if (meshId < 0)
            return false;
        sink.recordTranslatedInstance(part, meshId, ((RetainedModelManager.PartHandle)(Object)part).raynium$getMeshPage(), pose, tx, ty, tz, color, overlayCoords, lightCoords);
        return true;
    }

    public static final class GroupExtension {
        private final boolean canReorder;
        private final ArrayList<Object> entries;
        private int syncedFallbackCount;
        private @Nullable FastBucket lastFastBucket;
        private @Nullable RenderType lastFastRenderType;
        private final IdentityHashMap<RenderType, FastBucket> byRenderType;
        private final HashMap<PreparedRenderType, FastBucket> byPreparedType;

        GroupExtension(boolean canReorder, int existingFallbackCount) {
            this.canReorder = canReorder;
            entries = new ArrayList<>(Math.max(4, Math.addExact(existingFallbackCount, 1)));
            byRenderType = canReorder ? new IdentityHashMap<>(4) : null;
            byPreparedType = canReorder ? HashMap.newHashMap(4) : null;
            syncFallbackDraws(existingFallbackCount);
        }

        public void recycle() {
            for (Object entry : entries) {
                if (entry instanceof FastBucket bucket)
                    recycleFastBucket(bucket);
            }
            entries.clear();
            if (canReorder) {
                byRenderType.clear();
                byPreparedType.clear();
            }
            lastFastBucket = null;
            lastFastRenderType = null;
        }

        public void syncFallbackDraws(int totalFallbackCount) {
            int additional = totalFallbackCount - syncedFallbackCount;
            if (additional > 1)
                entries.ensureCapacity(Math.addExact(entries.size(), additional));
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
                var existing = byRenderType.get(renderType);
                if (existing == null)
                    existing = byPreparedType.get(prepared);
                if (existing != null) {
                    lastFastBucket = existing;
                    lastFastRenderType = renderType;
                    return existing;
                }
            }
            FastBucket bucket = ModelFastTrack.acquireFastBucket(prepared, consolidate);
            entries.add(bucket);
            if (consolidate) {
                byRenderType.put(renderType, bucket);
                byPreparedType.put(prepared, bucket);
            }
            lastFastBucket = bucket;
            lastFastRenderType = renderType;
            return bucket;
        }

        public void addShadow(ShadowFastTrack shadow) {
            entries.add(shadow);
        }

        public void execute(StagedVertexBuffer stagedBuffer, @Nullable OitStage stage, RenderPass renderPass,
                            List<StagedVertexBuffer.Draw> draws, List<PreparedRenderType> drawRenderTypes) {
            for (int i = 0; i < entries.size(); ++i) {
                Object entry = entries.get(i);
                if (entry instanceof FastBucket bucket) {
                    bucket.execute(stage, renderPass);
                } else if (entry instanceof ShadowFastTrack shadow) {
                    shadow.execute(stage, renderPass);
                } else {
                    int idx = ((Integer)entry).intValue();
                    PreparedRenderType renderType = drawRenderTypes.get(idx);
                    StagedVertexBuffer.ExecuteInfo info = stagedBuffer.getExecuteInfo(draws.get(idx));
                    if (info != null) {
                        if (stage != null)
                            renderType.drawFromBufferOit(info, stage, renderPass);
                        else
                            renderType.drawFromBuffer(info, renderPass);
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

    private static final class GroupingScratch {
        final MeshRunTable meshes = new MeshRunTable(), pages = new MeshRunTable();
        int[] instanceRun = new int[32];
        int[] runPages = new int[8], pageCursors = new int[8], groupedIds = new int[8],
              groupedCounts = new int[8], runCursors = new int[8];

        int peakInstances, quietExecutions;

        void begin(int instances) {
            peakInstances = Math.max(peakInstances, instances);
            meshes.reset();
            pages.reset();
            if (instanceRun.length < instances)
                instanceRun = new int[Math.max(instances, instanceRun.length * 2)];
        }

        void ensureRuns(int needed) {
            if (runPages.length >= needed)
                return;
            int cap = Math.max(needed, runPages.length * 2);
            runPages = Arrays.copyOf(runPages, cap);
            pageCursors = Arrays.copyOf(pageCursors, cap);
            groupedIds = new int[cap];
            groupedCounts = new int[cap];
            runCursors = new int[cap];
        }
    }

    static final class FastBucket {
        PreparedRenderType renderType;
        boolean canReorder;
        private ModelPart[] owners = new ModelPart[32];
        private int[] pageIds = new int[32];
        private int[] meshIds = new int[32];
        private int[] instanceInts = new int[32 * RetainedModelManager.INSTANCE_INTS];
        private int instanceCount;
        private @Nullable CommandTape uploadedTape;
        private long uploadedOffset;
        private int[] runMeshIds = new int[8];
        private int[] runFirstInstances = new int[8];
        private int[] runInstanceCounts = new int[8];
        private int[] ownerIndices = new int[8];
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
            if (uploadedTape == tape)
                return;
            long totalBytes = (long)instanceCount * RetainedModelManager.INSTANCE_BYTES;
            long offset = tape.reserveInstanceUpload(totalBytes, 8);
            MemorySegment dst = tape.instanceUploadSlice(offset, totalBytes);
            if (!canReorder || instanceCount == 1) {
                MemorySegment.copy(instanceInts, 0, dst, ValueLayout.JAVA_INT, 0, instanceCount * RetainedModelManager.INSTANCE_INTS);
                buildSequentialRuns();
            } else {
                buildGroupedAndCopy(dst);
            }
            tape.retainModels(owners, ownerIndices, runCount);
            uploadedTape = tape;
            uploadedOffset = offset;
        }

        private void buildSequentialRuns() {
            int runs = 0;
            int prev = -1;
            for (int i = 0; i < instanceCount; ++i) {
                if (meshIds[i] != prev) {
                    ++runs;
                    prev = meshIds[i];
                }
            }
            ensureRunCapacity(runs);
            runCount = 0;
            int start = 0;
            while (start < instanceCount) {
                int id = meshIds[start];
                int end = start + 1;
                while (end < instanceCount && meshIds[end] == id)
                    ++end;
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
                if (meshIds[i] != firstId) {
                    singleMesh = false;
                    break;
                }
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
            var scratch = grouping;
            scratch.begin(instanceCount);
            int runs = 0, pages = 0;
            for (int i = 0; i < instanceCount; ++i) {
                int id = meshIds[i];
                int r = scratch.meshes.getOrInsert(id, runs);
                if (r < 0) {
                    r = runs++;
                    ensureRunCapacity(runs);
                    scratch.ensureRuns(runs);
                    int page = scratch.pages.getOrInsert(pageIds[i], pages);
                    if (page < 0) {
                        page = pages++;
                        scratch.pageCursors[page] = 0;
                    }
                    scratch.runPages[r] = page;
                    ++scratch.pageCursors[page];
                    ownerIndices[r] = i;
                    runMeshIds[r] = id;
                    runInstanceCounts[r] = 1;
                } else {
                    ++runInstanceCounts[r];
                }
                scratch.instanceRun[i] = r;
            }
            if (pages > 1) {
                int start = 0;
                for (int p = 0; p < pages; ++p) {
                    int count = scratch.pageCursors[p];
                    scratch.pageCursors[p] = start;
                    start += count;
                }
                for (int r = 0; r < runs; ++r) {
                    int grouped = scratch.pageCursors[scratch.runPages[r]]++;
                    scratch.groupedIds[grouped] = runMeshIds[r];
                    scratch.groupedCounts[grouped] = runInstanceCounts[r];
                    scratch.runPages[r] = grouped;
                }
                System.arraycopy(scratch.groupedIds, 0, runMeshIds, 0, runs);
                System.arraycopy(scratch.groupedCounts, 0, runInstanceCounts, 0, runs);
                for (int i = 0; i < instanceCount; ++i)
                    scratch.instanceRun[i] = scratch.runPages[scratch.instanceRun[i]];
            }
            runCount = runs;
            int prefix = 0;
            for (int r = 0; r < runs; ++r) {
                runFirstInstances[r] = prefix;
                scratch.runCursors[r] = prefix;
                prefix += runInstanceCounts[r];
            }
            for (int i = 0; i < instanceCount; ++i) {
                int dstIndex = scratch.runCursors[scratch.instanceRun[i]]++;
                MemorySegment.copy(instanceInts, i * RetainedModelManager.INSTANCE_INTS,
                                   dst, ValueLayout.JAVA_INT, (long)dstIndex * RetainedModelManager.INSTANCE_BYTES,
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
            }
        }

        int runCount() {
            return runCount;
        }
        int runMeshId(int r) {
            return runMeshIds[r];
        }
        int runFirstInstance(int r) {
            return runFirstInstances[r];
        }
        int runInstanceCount(int r) {
            return runInstanceCounts[r];
        }

        void execute(@Nullable OitStage stage, RenderPass renderPass) {
            if (instanceCount == 0)
                return;
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
            fastBucket.addInstance(owner, meshId, pageId, pose, colorArgb, overlayCoords, lightCoords, null);
        }

        void recordTranslatedInstance(ModelPart owner, int meshId, int pageId, PoseStack.Pose pose, float tx, float ty, float tz,
                                      int colorArgb, int overlayCoords, int lightCoords) {
            GroupExtension ext = group.raynium$extension();
            if (fastBucket == null) {
                fastBucket = ext.acquireFastBucket(renderType, prepared);
            }
            fastBucket.addTranslatedInstance(owner, meshId, pageId, pose, tx, ty, tz, colorArgb, overlayCoords, lightCoords, null);
        }

        private VertexConsumer fallback() {
            if (fallback == null) {
                fallback = group.raynium$fallbackConsumer(renderType);
                fastBucket = null; // In strictlyOrdered mode, subsequent fast parts after fallback start a new bucket.
            }
            return fallback;
        }

        @Override
        public VertexConsumer addVertex(float x, float y, float z) {
            return fallback().addVertex(x, y, z);
        }
        @Override
        public VertexConsumer setColor(int r, int g, int b, int a) {
            return fallback().setColor(r, g, b, a);
        }
        @Override
        public VertexConsumer setColor(int color) {
            return fallback().setColor(color);
        }
        @Override
        public VertexConsumer setUv(float u, float v) {
            return fallback().setUv(u, v);
        }
        @Override
        public VertexConsumer setUv1(int u, int v) {
            return fallback().setUv1(u, v);
        }
        @Override
        public VertexConsumer setUv2(int u, int v) {
            return fallback().setUv2(u, v);
        }
        @Override
        public VertexConsumer setUv3(float u, float v) {
            return fallback().setUv3(u, v);
        }
        @Override
        public VertexConsumer setNormal(float x, float y, float z) {
            return fallback().setNormal(x, y, z);
        }
        @Override
        public VertexConsumer setLineWidth(float width) {
            return fallback().setLineWidth(width);
        }
        @Override
        public void addVertex(float x, float y, float z, int color, float u, float v, int overlayCoords, int lightCoords, float nx, float ny, float nz) {
            fallback().addVertex(x, y, z, color, u, v, overlayCoords, lightCoords, nx, ny, nz);
        }
    }
}
