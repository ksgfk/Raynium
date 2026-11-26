package dev.raynium.client.terrain;

import it.unimi.dsi.fastutil.ints.IntArrayList;
import it.unimi.dsi.fastutil.objects.ObjectArrayList;
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
    private static final AtomicLong EPOCH=new AtomicLong(1);
    private static final AtomicLong VISIBLE_LIST_VERSION=new AtomicLong(1);
    private static final ConcurrentLinkedQueue<SectionRenderDispatcher.RenderSection> DIRTY_SECTIONS=new ConcurrentLinkedQueue<>();
    private static long captureGeneration;
    private static final ConcurrentLinkedQueue<SectionPayload> REMOVED=new ConcurrentLinkedQueue<>();
    private static final ConcurrentLinkedQueue<SectionPayload> DIRTY_INDICES=new ConcurrentLinkedQueue<>();
    private static final Buffer BUFFER=new Buffer();
    private static final Matrix4f capturedProjection=new Matrix4f();
    private static boolean hasProjection,hasSnapshot,hasWarmSnapshot,capturedSinceBeginFrame;
    private static long capturedVisibleVersion=-1,capturedFadeDuration=-1;
    private static LevelRenderer capturedRenderer;
    private static volatile boolean active;

    /** Borrowed on the render thread until invalidation, deactivation, or the next capture. */
    static final class Buffer {
        SectionPayload[] payloads=new SectionPayload[1024];
        int[] originX=new int[1024],originY=new int[1024],originZ=new int[1024];
        float[] visibility=new float[1024];
        int[] elementMask=new int[1024],distKey=new int[1024];
        MeshPayload.IndexSnapshot[] indices=new MeshPayload.IndexSnapshot[1024];
        final IntArrayList uploadCandidates=new IntArrayList();
        final IntArrayList fadingSlots=new IntArrayList();
        final ObjectArrayList<SectionRenderDispatcher.RenderSection> fadingSections=new ObjectArrayList<>();
        final IntArrayList updatedFadingSlots=new IntArrayList();
        int[] candidateStamp=new int[1024];
        int generation=1;
        boolean[] fading=new boolean[1024];
        long[] uploadedAt=new long[1024];
        SectionRenderDispatcher.RenderSection[] capturedSections=new SectionRenderDispatcher.RenderSection[1024];
        long checkedResidencyEpoch;
        double camX,camY,camZ;
        boolean hasExplicitDistKeys;
        boolean fastReused,partialRefresh;
        boolean instancesChanged;
        int count;
        long epoch;
        final float[] projection=new float[16],view=new float[16],fraction=new float[3],fog=new float[4],distances=new float[4];
        final int[] block=new int[3];
        boolean rgss,improvedTransparency;
        int anisotropy;
        void ensureCapacity(int needed) {
            if(needed<=payloads.length) return;
            int capacity=payloads.length;
            while(capacity<needed) capacity=Math.multiplyExact(capacity,2);
            candidateStamp=Arrays.copyOf(candidateStamp,capacity);
            fading=Arrays.copyOf(fading,capacity);
            uploadedAt=Arrays.copyOf(uploadedAt,capacity);
            capturedSections=Arrays.copyOf(capturedSections,capacity);
            payloads=Arrays.copyOf(payloads,capacity); indices=Arrays.copyOf(indices,capacity);
            originX=Arrays.copyOf(originX,capacity); originY=Arrays.copyOf(originY,capacity); originZ=Arrays.copyOf(originZ,capacity);
            visibility=Arrays.copyOf(visibility,capacity); elementMask=Arrays.copyOf(elementMask,capacity); distKey=Arrays.copyOf(distKey,capacity);
        }
        void put(int i,SectionPayload payload,int x,int y,int z,float fade,int mask,int distance) {
            ensureCapacity(i+1);
            payload.capturedSlot=i; payloads[i]=payload; indices[i]=payload.layers[2]==null?null:payload.layers[2].indices; originX[i]=x; originY[i]=y; originZ[i]=z;
            visibility[i]=fade; elementMask[i]=mask; distKey[i]=distance; hasExplicitDistKeys=true;
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
        void resetFading() {
            for(int i=0;i<fadingSlots.size();i++) fading[fadingSlots.getInt(i)]=false;
            fadingSlots.clear();
            fadingSections.clear();
        }
        void addFading(int slot,SectionRenderDispatcher.RenderSection section) {
            if(!fading[slot]) {
                fading[slot]=true;
                fadingSlots.add(slot);
                fadingSections.add(section);
            }
        }
        void finish(int size) { if(size<count) { Arrays.fill(payloads,size,count,null); Arrays.fill(indices,size,count,null); } count=size; }
        void clear() {
            finish(0);
            checkedResidencyEpoch=0;
            resetCandidates();
            resetFading();
            updatedFadingSlots.clear();
            hasExplicitDistKeys=false;
            fastReused=false;
            instancesChanged=false;
        }
    }
    public static boolean active() { return active; }
    public static void activate(boolean value) { active=value; if(!value) { clear(); REMOVED.clear(); DIRTY_INDICES.clear(); } }
    public static long epoch() { return EPOCH.get(); }
    public static void bumpVisibleListVersion() { VISIBLE_LIST_VERSION.incrementAndGet(); }
    public static void sectionChanged(SectionRenderDispatcher.RenderSection section) {
        if(active) DIRTY_SECTIONS.add(section);
    }
    public static void recordDirtyIndices(SectionPayload payload) { if(active() && payload!=null) DIRTY_INDICES.add(payload); }
    static void drainDirtyIndices(Collection<SectionPayload> target) {
        SectionPayload value;
        while((value=DIRTY_INDICES.poll())!=null) target.add(value);
    }
    static void clearDirtyIndices() { DIRTY_INDICES.clear(); }
    private static void clear() {
        BUFFER.clear();
        hasSnapshot=false;
        hasProjection=false;
        hasWarmSnapshot=false;
        capturedSinceBeginFrame=false;
        capturedVisibleVersion=-1;
        DIRTY_SECTIONS.clear();
        capturedFadeDuration=-1;
        capturedRenderer=null;
    }
    public static void invalidate() {
        EPOCH.incrementAndGet();
        bumpVisibleListVersion();
        clear();
        REMOVED.clear();
        DIRTY_INDICES.clear();
    }
    public static void beginFrame() {
        hasProjection=false;
        hasSnapshot=false;
        if(!capturedSinceBeginFrame) {
            DIRTY_SECTIONS.clear();
            BUFFER.clear();
            hasWarmSnapshot=false;
            capturedRenderer=null;
        }
        capturedSinceBeginFrame=false;
    }
    public static void projection(Matrix4f value) { if(active()) { capturedProjection.set(value); hasProjection=true; } }
    public static void removed(SectionPayload payload) { if(active() && payload!=null) REMOVED.add(payload); }
    public static void drainRemoved(Collection<SectionPayload> target) {
        SectionPayload value;
        while((value=REMOVED.poll())!=null) target.add(value);
    }
    static Buffer snapshot() { return hasSnapshot?BUFFER:null; }
    private static void updateCameraState(CameraRenderState camera,OptionsRenderState options,boolean improvedTransparency) {
        BUFFER.improvedTransparency=improvedTransparency;
        capturedProjection.get(BUFFER.projection); camera.viewRotationMatrix.get(BUFFER.view);
        var p=camera.pos; var b=camera.blockPos; var f=camera.fogData;
        BUFFER.camX=p.x-8.0; BUFFER.camY=p.y-8.0; BUFFER.camZ=p.z-8.0;
        BUFFER.block[0]=b.getX(); BUFFER.block[1]=b.getY(); BUFFER.block[2]=b.getZ();
        BUFFER.fraction[0]=(float)(p.x-b.getX()); BUFFER.fraction[1]=(float)(p.y-b.getY()); BUFFER.fraction[2]=(float)(p.z-b.getZ());
        BUFFER.fog[0]=f.color.x; BUFFER.fog[1]=f.color.y; BUFFER.fog[2]=f.color.z; BUFFER.fog[3]=f.color.w;
        BUFFER.distances[0]=f.environmentalStart; BUFFER.distances[1]=f.environmentalEnd;
        BUFFER.distances[2]=f.renderDistanceStart; BUFFER.distances[3]=f.renderDistanceEnd;
        BUFFER.rgss=options.textureFiltering==net.minecraft.client.TextureFilteringMethod.RGSS;
        BUFFER.anisotropy=options.textureFiltering==net.minecraft.client.TextureFilteringMethod.ANISOTROPIC?options.maxAnisotropyValue:1;
    }
    static void capture(LevelRenderer renderer,CameraRenderState camera,OptionsRenderState options,boolean improvedTransparency,long residencyEpoch) {
        if(!active() || !hasProjection) return;
        long epoch=epoch(),fade=Util.toMillis(options.chunkSectionFadeInTime);
        long visVer=VISIBLE_LIST_VERSION.get();
        boolean dirty=!DIRTY_SECTIONS.isEmpty();
        if(hasWarmSnapshot && capturedRenderer==renderer && BUFFER.epoch==epoch &&
           visVer==capturedVisibleVersion && fade==capturedFadeDuration &&
           (dirty ? refreshDirty(epoch,fade) : REMOVED.isEmpty())) {
            BUFFER.updatedFadingSlots.clear();
            boolean instChanged=false;
            if(!BUFFER.fadingSlots.isEmpty()) {
                long now=Util.getMillis();
                var visArr=BUFFER.visibility;
                for(int i=BUFFER.fadingSlots.size()-1;i>=0;i--) {
                    int slot=BUFFER.fadingSlots.getInt(i);
                    var section=BUFFER.fadingSections.get(i);
                    float v=fade<=0L?1.0f:section.getVisibility(now,fade);
                    if(v!=visArr[slot]) {
                        visArr[slot]=v;
                        BUFFER.updatedFadingSlots.add(slot);
                        instChanged=true;
                    }
                    if(v>=1.0f) {
                        BUFFER.fading[slot]=false;
                        int last=BUFFER.fadingSlots.size()-1;
                        if(i!=last) {
                            BUFFER.fadingSlots.set(i,BUFFER.fadingSlots.getInt(last));
                            BUFFER.fadingSections.set(i,BUFFER.fadingSections.get(last));
                        }
                        BUFFER.fadingSlots.removeInt(last);
                        BUFFER.fadingSections.remove(last);
                    }
                }
            }
            BUFFER.fastReused=!dirty;
            BUFFER.instancesChanged=instChanged;
            BUFFER.checkedResidencyEpoch=dirty?residencyEpoch:0;
            BUFFER.partialRefresh=dirty;
            updateCameraState(camera,options,improvedTransparency);
            hasSnapshot=true;
            capturedSinceBeginFrame=true;
            return;
        }
        var dispatcher=renderer.sectionRenderDispatcher();
        if(dispatcher==null) return;
        DIRTY_SECTIONS.clear();
        DIRTY_INDICES.clear();
        ++captureGeneration;
        long now=fade<=0L?0L:Util.getMillis();
        int count=0;
        BUFFER.resetCandidates();
        BUFFER.resetFading();
        BUFFER.updatedFadingSlots.clear();
        BUFFER.hasExplicitDistKeys=false;
        BUFFER.fastReused=false;
        BUFFER.partialRefresh=false;
        BUFFER.instancesChanged=true;
        dispatcher.lock();
        try {
            var sections=renderer.visibleSections();
            int sectionCount=sections.size();
            BUFFER.ensureCapacity(sectionCount);
            var payloads=BUFFER.payloads;
            var indices=BUFFER.indices;
            var originX=BUFFER.originX;
            var originY=BUFFER.originY;
            var originZ=BUFFER.originZ;
            var elementMask=BUFFER.elementMask;
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
                long uploaded=section.uploadedTime;
                BUFFER.uploadedAt[slot]=uploaded;
                if(fade>0 && now-uploaded<fade) BUFFER.capturedSections[slot]=section;
                elementMask[slot]=mask;
            }
        } finally { dispatcher.unlock(); }
        for(int slot=0;slot<count;slot++) {
            var payload=BUFFER.payloads[slot];
            if(payload.uploadedEpoch!=residencyEpoch || BUFFER.indices[slot]!=payload.uploadedIndices)
                BUFFER.addCandidate(slot);
            long elapsed=now-BUFFER.uploadedAt[slot];
            float visibility=fade<=0 || elapsed>=fade?1f:(float)elapsed/fade;
            BUFFER.visibility[slot]=visibility;
            if(fade>0 && elapsed<fade) {
                if(visibility<1f) BUFFER.addFading(slot,BUFFER.capturedSections[slot]);
                BUFFER.capturedSections[slot]=null;
            }
        }
        BUFFER.finish(count); BUFFER.epoch=epoch;
        BUFFER.checkedResidencyEpoch=residencyEpoch;
        updateCameraState(camera,options,improvedTransparency);
        capturedVisibleVersion=visVer;
        capturedFadeDuration=fade;
        capturedRenderer=renderer;
        hasWarmSnapshot=true;
        hasSnapshot=true;
        capturedSinceBeginFrame=true;
    }
    static boolean refreshDirty(long epoch,long fade) {
        long now=fade<=0?0:Util.getMillis();
        BUFFER.resetCandidates();
        int limit=Math.max(256,BUFFER.count>>>2);
        for(int changed=0;;++changed) {
            var section=DIRTY_SECTIONS.poll();
            if(section==null) {
                var indices=DIRTY_INDICES.poll();
                if(indices==null) return true;
                if(changed>=limit) return false;
                BUFFER.refreshIndexCandidate(indices);
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
            BUFFER.payloads[slot]=payload;
            BUFFER.elementMask[slot]=state.readyMask();
            var tr=payload.layers[2];
            BUFFER.indices[slot]=tr==null?null:tr.indices;
            var origin=section.getRenderOrigin();
            BUFFER.originX[slot]=origin.getX();
            BUFFER.originY[slot]=origin.getY();
            BUFFER.originZ[slot]=origin.getZ();
            float visibility=fade<=0?1:section.getVisibility(now,fade);
            BUFFER.visibility[slot]=visibility;
            if(visibility<1) BUFFER.addFading(slot,section);
            BUFFER.addCandidate(slot);
        }
    }

    static int distanceKey(double squared) {
        float distance=(float)squared;
        return Float.floatToRawIntBits(Float.isNaN(distance)?Float.POSITIVE_INFINITY:Math.max(0,distance));
    }
}
