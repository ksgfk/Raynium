package dev.raynium.client.terrain;

import it.unimi.dsi.fastutil.ints.IntArrayList;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import net.minecraft.client.renderer.state.OptionsRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.util.Util;
import org.joml.Matrix4f;
import java.util.Arrays;
import java.util.Collection;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicLong;

public final class TerrainCapture {
    private static final long TIME_ORIGIN_MS=Util.getMillis();
    private static final class Events {
        final long epoch;
        final ConcurrentLinkedQueue<SectionRenderDispatcher.RenderSection> sections=new ConcurrentLinkedQueue<>();
        final ConcurrentLinkedQueue<SectionPayload> removed=new ConcurrentLinkedQueue<>(), indices=new ConcurrentLinkedQueue<>();
        volatile boolean trackingSections;
        Events(long epoch) { this.epoch=epoch; }
    }
    private static volatile Events events=new Events(1);
    private static final AtomicLong VISIBLE_LIST_VERSION=new AtomicLong(1);
    private static long captureGeneration;
    private static Buffer buffer=new Buffer();
    private static final Matrix4f capturedProjection=new Matrix4f();
    private static boolean hasProjection,hasSnapshot,hasWarmSnapshot,capturedSinceBeginFrame;
    private static long capturedVisibleVersion=-1;
    private static LevelRenderer capturedRenderer;
    private static volatile boolean active;

    /** Borrowed on the render thread until invalidation, deactivation, or the next capture. */
    static final class Buffer {
        SectionPayload[] payloads=new SectionPayload[1024];
        int[] originX=new int[1024],originY=new int[1024],originZ=new int[1024];
        int[] fadeStart=new int[1024];
        int fadeNowMs,fadeDurationMs;
        int[] elementMask=new int[1024],distKey=new int[1024];
        MeshPayload.IndexSnapshot[] indices=new MeshPayload.IndexSnapshot[1024];
        final IntArrayList uploadCandidates=new IntArrayList();
        int[] candidateStamp=new int[1024];
        int generation=1;
        long checkedResidencyEpoch;
        double camX,camY,camZ;
        boolean hasExplicitDistKeys;
        boolean fastReused,partialRefresh;
        int count;
        long epoch;
        final dev.raynium.bridge.NativeRenderer.Frame frame=new dev.raynium.bridge.NativeRenderer.Frame();
        final float[] projection=frame.projection,view=frame.view,fraction=frame.cameraFraction,fog=frame.fogColor,distances=frame.fogDistances;
        final int[] block=frame.cameraBlock;
        boolean rgss,improvedTransparency;
        int anisotropy;
        void ensureCapacity(int needed) {
            if(needed<=payloads.length) return;
            int capacity=payloads.length;
            while(capacity<needed) capacity=Math.multiplyExact(capacity,2);
            candidateStamp=Arrays.copyOf(candidateStamp,capacity);
            payloads=Arrays.copyOf(payloads,capacity); indices=Arrays.copyOf(indices,capacity);
            originX=Arrays.copyOf(originX,capacity); originY=Arrays.copyOf(originY,capacity); originZ=Arrays.copyOf(originZ,capacity);
            fadeStart=Arrays.copyOf(fadeStart,capacity); elementMask=Arrays.copyOf(elementMask,capacity); distKey=Arrays.copyOf(distKey,capacity);
        }
        void put(int i,SectionPayload payload,int x,int y,int z,int startMs,int mask,int distance) {
            ensureCapacity(i+1);
            payload.capturedSlot=i; payloads[i]=payload; indices[i]=payload.layers[2]==null?null:payload.layers[2].indices; originX[i]=x; originY[i]=y; originZ[i]=z;
            fadeStart[i]=startMs; elementMask[i]=mask; distKey[i]=distance; hasExplicitDistKeys=true;
        }
        boolean contains(SectionPayload payload) {
            int slot=payload.capturedSlot;
            return slot>=0 && slot<count && payloads[slot]==payload;
        }
        void refreshIndexCandidate(SectionPayload payload) {
            if(!contains(payload)) return;
            int slot=payload.capturedSlot;
            var layer=payload.layers[2];
            indices[slot]=layer==null?null:layer.indices;
            addCandidate(slot);
        }
        void addCandidate(int slot) {
            if(candidateStamp[slot]!=generation) {
                candidateStamp[slot]=generation;
                uploadCandidates.add(slot);
            }
        }
        void resetCandidates() {
            uploadCandidates.clear();
            if(++generation==0) { Arrays.fill(candidateStamp,0); generation=1; }
        }
        void finish(int size) { if(size<count) { Arrays.fill(payloads,size,count,null); Arrays.fill(indices,size,count,null); } count=size; }
        void clear() {
            finish(0);
            checkedResidencyEpoch=0;
            resetCandidates();
            hasExplicitDistKeys=false;
            fastReused=false;
        }
    }
    // Zero disables fading once the process-relative unsigned clock has reached its horizon.
    static int encodeRelativeMillis(long elapsed) {
        return elapsed>=0xffff_fffeL?0:(int)(Math.max(0L,elapsed)+1);
    }
    static int encodeFadeStartMs(long uploadedTime) {
        return uploadedTime==0?0:encodeRelativeMillis(uploadedTime-TIME_ORIGIN_MS);
    }
    public static boolean active() { return active; }
    public static void activate(boolean value) {
        boolean retiring=active && !value;
        active=value;
        if(retiring) invalidate();
    }
    public static long epoch() { return events.epoch; }
    public static void bumpVisibleListVersion() { VISIBLE_LIST_VERSION.incrementAndGet(); }
    public static void sectionChanged(SectionRenderDispatcher.RenderSection section) {
        var current=events;
        if(active && current.trackingSections) current.sections.add(section);
    }
    public static void recordDirtyIndices(SectionPayload payload) {
        var current=events;
        if(active && payload!=null && payload.epoch==current.epoch) current.indices.add(payload);
    }
    static void drainDirtyIndices(Collection<SectionPayload> target) {
        SectionPayload value;
        while((value=events.indices.poll())!=null) target.add(value);
    }
    static void clearDirtyIndices() { events.indices.clear(); }
    private static void clear() {
        buffer.clear();
        hasSnapshot=false;
        hasProjection=false;
        hasWarmSnapshot=false;
        capturedSinceBeginFrame=false;
        capturedVisibleVersion=-1;
        events.sections.clear();
        capturedRenderer=null;
    }
    public static void invalidate() {
        long nextEpoch=events.epoch+1;
        DirectUploadPool.retire(nextEpoch);
        events=new Events(nextEpoch);
        bumpVisibleListVersion();
        clear();
        buffer=new Buffer();
    }
    public static void beginFrame() {
        hasProjection=false;
        hasSnapshot=false;
        if(!capturedSinceBeginFrame) {
            events.sections.clear();
            buffer.clear();
            hasWarmSnapshot=false;
            capturedRenderer=null;
        }
        capturedSinceBeginFrame=false;
    }
    public static void projection(Matrix4f value) { if(active()) { capturedProjection.set(value); hasProjection=true; } }
    public static void removed(SectionPayload payload) {
        var current=events;
        if(active && payload!=null && payload.epoch==current.epoch) current.removed.add(payload);
    }
    public static void drainRemoved(Collection<SectionPayload> target) {
        SectionPayload value;
        while((value=events.removed.poll())!=null) target.add(value);
    }
    static Buffer snapshot() { return hasSnapshot?buffer:null; }
    private static void updateCameraState(CameraRenderState camera,OptionsRenderState options,boolean improvedTransparency) {
        buffer.improvedTransparency=improvedTransparency;
        capturedProjection.get(buffer.projection); camera.viewRotationMatrix.get(buffer.view);
        var p=camera.pos; var b=camera.blockPos; var f=camera.fogData;
        buffer.camX=p.x-8.0; buffer.camY=p.y-8.0; buffer.camZ=p.z-8.0;
        buffer.block[0]=b.getX(); buffer.block[1]=b.getY(); buffer.block[2]=b.getZ();
        buffer.fraction[0]=(float)(p.x-b.getX()); buffer.fraction[1]=(float)(p.y-b.getY()); buffer.fraction[2]=(float)(p.z-b.getZ());
        buffer.fog[0]=f.color.x; buffer.fog[1]=f.color.y; buffer.fog[2]=f.color.z; buffer.fog[3]=f.color.w;
        buffer.distances[0]=f.environmentalStart; buffer.distances[1]=f.environmentalEnd;
        buffer.distances[2]=f.renderDistanceStart; buffer.distances[3]=f.renderDistanceEnd;
        buffer.rgss=options.textureFiltering==net.minecraft.client.TextureFilteringMethod.RGSS;
        buffer.anisotropy=options.textureFiltering==net.minecraft.client.TextureFilteringMethod.ANISOTROPIC?options.maxAnisotropyValue:1;
    }
    static void capture(LevelRenderer renderer,CameraRenderState camera,OptionsRenderState options,boolean improvedTransparency,long residencyEpoch) {
        if(!active() || !hasProjection) return;
        long epoch=epoch();
        buffer.fadeNowMs=encodeRelativeMillis(Util.getMillis()-TIME_ORIGIN_MS);
        buffer.fadeDurationMs=(int)Math.clamp(Util.toMillis(options.chunkSectionFadeInTime),0L,0xffff_ffffL);
        long visVer=VISIBLE_LIST_VERSION.get();
        boolean dirty=!events.sections.isEmpty();
        if(hasWarmSnapshot && capturedRenderer==renderer && buffer.epoch==epoch &&
           visVer==capturedVisibleVersion &&
           (dirty ? refreshDirty(epoch) : events.removed.isEmpty())) {
            buffer.fastReused=!dirty;
            buffer.checkedResidencyEpoch=dirty?residencyEpoch:0;
            buffer.partialRefresh=dirty;
            updateCameraState(camera,options,improvedTransparency);
            hasSnapshot=true;
            capturedSinceBeginFrame=true;
            return;
        }
        var dispatcher=renderer.sectionRenderDispatcher();
        if(dispatcher==null) return;
        events.sections.clear();
        events.indices.clear();
        events.trackingSections=true;
        ++captureGeneration;
        int count=0;
        buffer.resetCandidates();
        buffer.hasExplicitDistKeys=false;
        buffer.fastReused=false;
        buffer.partialRefresh=false;
        dispatcher.lock();
        try {
            var sections=renderer.visibleSections();
            int sectionCount=sections.size();
            buffer.ensureCapacity(sectionCount);
            var payloads=buffer.payloads;
            var indices=buffer.indices;
            var originX=buffer.originX;
            var originY=buffer.originY;
            var originZ=buffer.originZ;
            var elementMask=buffer.elementMask;
            for(int i=0;i<sectionCount;i++) {
                var section=sections.get(i);
                var access=(SectionStateAccess)(Object)section;
                var state=access.raynium$state();
                var payload=state==null?null:state.payload();
                if(payload==null || payload.epoch!=epoch) {
                    access.raynium$setCaptureSlot(captureGeneration,-1);
                    continue;
                }
                access.raynium$setCaptureSlot(captureGeneration,count);
                int mask=state.readyMask();
                var origin=section.getRenderOrigin();
                int slot=count++;
                payload.capturedSlot=slot;
                payloads[slot]=payload;
                var translucent=payload.layers[2];
                var indexSnapshot=translucent==null?null:translucent.indices;
                indices[slot]=indexSnapshot;
                originX[slot]=origin.getX(); originY[slot]=origin.getY(); originZ[slot]=origin.getZ();
                buffer.fadeStart[slot]=encodeFadeStartMs(section.uploadedTime);
                elementMask[slot]=mask;
            }
        } finally { dispatcher.unlock(); }
        for(int slot=0;slot<count;slot++) {
            var payload=buffer.payloads[slot];
            if(payload.uploadedEpoch!=residencyEpoch || buffer.indices[slot]!=payload.uploadedIndices)
                buffer.addCandidate(slot);
        }
        buffer.finish(count); buffer.epoch=epoch;
        buffer.checkedResidencyEpoch=residencyEpoch;
        updateCameraState(camera,options,improvedTransparency);
        capturedVisibleVersion=visVer;
        capturedRenderer=renderer;
        hasWarmSnapshot=true;
        hasSnapshot=true;
        capturedSinceBeginFrame=true;
    }
    static boolean refreshDirty(long epoch) {
        buffer.resetCandidates();
        int limit=Math.max(256,buffer.count>>>2);
        for(int changed=0;;++changed) {
            var section=events.sections.poll();
            if(section==null) {
                var indices=events.indices.poll();
                if(indices==null) return true;
                if(changed>=limit) return false;
                buffer.refreshIndexCandidate(indices);
                continue;
            }
            if(changed>=limit) return false;
            var access=(SectionStateAccess)(Object)section;
            if(access.raynium$captureGen()!=captureGeneration) continue;
            int slot=access.raynium$captureSlot();
            var state=access.raynium$state();
            var payload=state==null?null:state.payload();
            boolean present=payload!=null && payload.epoch==epoch;
            if(present!=(slot>=0)) return false;
            if(!present) continue;
            payload.capturedSlot=slot;
            buffer.payloads[slot]=payload;
            buffer.elementMask[slot]=state.readyMask();
            var tr=payload.layers[2];
            buffer.indices[slot]=tr==null?null:tr.indices;
            var origin=section.getRenderOrigin();
            buffer.originX[slot]=origin.getX();
            buffer.originY[slot]=origin.getY();
            buffer.originZ[slot]=origin.getZ();
            buffer.fadeStart[slot]=encodeFadeStartMs(section.uploadedTime);
            buffer.addCandidate(slot);
        }
    }

    static int distanceKey(double squared) {
        float distance=(float)squared;
        return Float.floatToRawIntBits(Float.isNaN(distance)?Float.POSITIVE_INFINITY:Math.max(0,distance));
    }
}
