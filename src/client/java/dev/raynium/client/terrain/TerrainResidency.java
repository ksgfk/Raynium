package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeFrameBuffer;
import dev.raynium.bridge.NativeRenderer;
import it.unimi.dsi.fastutil.ints.IntArrayList;
import it.unimi.dsi.fastutil.ints.IntArrays;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import it.unimi.dsi.fastutil.objects.ObjectArrayList;
import java.lang.foreign.MemorySegment;
import java.nio.ByteOrder;
import java.util.Arrays;
import net.minecraft.core.SectionPos;
import static java.lang.foreign.ValueLayout.*;

/** Render-thread residency. Commit only after a successful native submission. */
final class TerrainResidency {
    static final long UPLOAD_BUDGET=3L*1024*1024;
    static final Runnable NOOP=()->{};
    static {
        if(ByteOrder.nativeOrder()!=ByteOrder.LITTLE_ENDIAN)
            throw new IllegalStateException("Raynium requires little-endian native byte order");
    }
    private final Long2ObjectOpenHashMap<SectionPayload> residentBySection=new Long2ObjectOpenHashMap<>(4096);
    final ObjectArrayList<SectionPayload> pendingRemovals=new ObjectArrayList<>();
    private final IntArrayList candidates=new IntArrayList(1024),staged=new IntArrayList(1024);
    private final IntArrayList freeSlots=new IntArrayList(1024);
    private final ObjectArrayList<MeshPayload> pendingSlotAllocs=new ObjectArrayList<>();
    private final ObjectArrayList<SectionPayload> evicting=new ObjectArrayList<>();
    private record IndexUpload(SectionPayload section,MeshPayload.IndexSnapshot snapshot) {}
    private final ObjectArrayList<IndexUpload> indexUploads=new ObjectArrayList<>();
    private final ObjectArrayList<SectionPayload> dirtyIndicesScratch=new ObjectArrayList<>();
    private int[] instScratch=new int[4096*4];
    private int[] visScratch=new int[4096*6];
    private int[] sectionToInstance=new int[4096];
    private TerrainCapture.Buffer capture;
    private long epoch,frameId;
    private int residentCount;
    private int nextSlotId=1;
    private long visibleVersion,originsVersion,instancesVersion;
    private int lastVisCount,lastInstCount;
    private int lastReuseFlags;
    private boolean hasDeferredUploads;
    private boolean lastFrameCommitted,lastInstancesCommitted;
    private boolean uncommittedPrepare,pendingAllowReuse;
    long uploadBytes;

    void reset(long epoch) {
        for(int i=0;i<pendingSlotAllocs.size();i++) pendingSlotAllocs.get(i).slotId=0;
        for(var section:residentBySection.values()) {
            for(var layer:section.layers) if(layer!=null) layer.slotId=0;
        }
        residentBySection.clear(); pendingRemovals.clear(); staged.clear(); evicting.clear();
        candidates.clear(); indexUploads.clear(); freeSlots.clear(); pendingSlotAllocs.clear();
        dirtyIndicesScratch.clear();
        capture=null; this.epoch=epoch; residentCount=0; nextSlotId=1;
        visibleVersion=0; originsVersion=0; instancesVersion=0;
        lastVisCount=0; lastInstCount=0; lastReuseFlags=0;
        hasDeferredUploads=false; lastFrameCommitted=false; lastInstancesCommitted=false; uncommittedPrepare=false;
    }
    private int allocateSlot() {
        return freeSlots.isEmpty()?nextSlotId++:freeSlots.popInt();
    }
    private void rollbackPendingSlots() {
        if(pendingSlotAllocs.isEmpty()) return;
        for(int i=0;i<pendingSlotAllocs.size();i++) {
            var layer=pendingSlotAllocs.get(i);
            if(layer.slotId!=0) { freeSlots.push(layer.slotId); layer.slotId=0; }
        }
        pendingSlotAllocs.clear();
    }
    private void ensureScratchCapacity(int sectionCount) {
        int neededInst=Math.multiplyExact(Math.max(1024,sectionCount),4);
        if(instScratch.length<neededInst) {
            int cap=instScratch.length;
            while(cap<neededInst) cap=Math.multiplyExact(cap,2);
            instScratch=new int[cap];
        }
        int neededVis=Math.multiplyExact(Math.max(1024,sectionCount),18); // up to 3 layers * 6 ints
        if(visScratch.length<neededVis) {
            int cap=visScratch.length;
            while(cap<neededVis) cap=Math.multiplyExact(cap,2);
            visScratch=new int[cap];
        }
        if(sectionToInstance.length<sectionCount) {
            int cap=sectionToInstance.length;
            while(cap<sectionCount) cap=Math.multiplyExact(cap,2);
            sectionToInstance=new int[cap];
        }
    }
    private static <T> void swapPop(ObjectArrayList<T> list,int index) {
        int last=list.size()-1;
        if(index!=last) list.set(index,list.get(last));
        list.remove(last);
    }
    private void emitIndexUploads(NativeFrameBuffer frame) {
        for(int i=0;i<indexUploads.size();i++) {
            var update=indexUploads.get(i);
            long slotId=Integer.toUnsignedLong(update.section.layers[2].slotId);
            frame.indexPooled(slotId,update.snapshot);
        }
    }
    int reuseFlags() { return lastReuseFlags; }
    int effectiveVisibleCount() { return lastVisCount; }
    void prepare(TerrainCapture.Buffer capture,NativeFrameBuffer frame,long albedo,long lightmap) {
        prepare(capture,frame,albedo,lightmap,false);
    }
    void prepare(TerrainCapture.Buffer capture,NativeFrameBuffer frame,long albedo,long lightmap,boolean allowReuse) {
        if(uncommittedPrepare) {
            lastFrameCommitted=false;
            lastInstancesCommitted=false;
        }
        rollbackPendingSlots();
        uncommittedPrepare=true;
        pendingAllowReuse=allowReuse;
        this.capture=capture; frameId=NativeRenderer.nextFrameId();
        candidates.clear(); staged.clear(); evicting.clear(); indexUploads.clear();
        uploadBytes=0;
        lastReuseFlags=0;

        boolean canReuse=allowReuse && capture.fastReused && lastFrameCommitted && !hasDeferredUploads && pendingRemovals.isEmpty();
        if(canReuse) {
            dirtyIndicesScratch.clear();
            TerrainCapture.drainDirtyIndices(dirtyIndicesScratch);
            for(int i=0;i<dirtyIndicesScratch.size();i++) {
                var payload=dirtyIndicesScratch.get(i);
                if(payload.uploadedEpoch!=epoch && capture.contains(payload)) { canReuse=false; break; }
            }
        }
        if(canReuse) {
            for(int i=0;i<dirtyIndicesScratch.size();i++) {
                var p=dirtyIndicesScratch.get(i);
                if(p.uploadedEpoch!=epoch || p.layers[2]==null) continue;
                var idx=p.layers[2].indices;
                if(capture.contains(p)) capture.indices[p.capturedSlot]=idx;
                if(idx!=null && idx!=p.uploadedIndices && idx.hasData() && p.dirtyIndexFrame!=frameId) {
                    p.dirtyIndexFrame=frameId;
                    indexUploads.add(new IndexUpload(p,idx));
                    uploadBytes+=idx.byteSize();
                }
            }
            boolean activeInstancesChanged=false;
            if(capture.instancesChanged) {
                for(int i=0;i<capture.updatedFadingSlots.size();i++) {
                    int slot=capture.updatedFadingSlots.getInt(i);
                    if(slot>=0 && slot<sectionToInstance.length) {
                        int instIdx=sectionToInstance[slot];
                        if(instIdx>=0 && instIdx<lastInstCount) {
                            int bits=Float.floatToRawIntBits(capture.visibility[slot]);
                            int off=instIdx*4+3;
                            if(instScratch[off]!=bits) {
                                instScratch[off]=bits;
                                activeInstancesChanged=true;
                            }
                        }
                    }
                }
                if(activeInstancesChanged) instancesVersion=frameId;
            }
            if(!activeInstancesChanged && lastInstancesCommitted) {
                lastReuseFlags=NativeRenderer.REUSE_VISIBLE|NativeRenderer.REUSE_INSTANCES;
                frame.begin(0,0,0,indexUploads.size(),0);
                frame.terrain(albedo,lightmap,capture.anisotropy);
                frame.versions(visibleVersion,originsVersion,instancesVersion);
                emitIndexUploads(frame);
                frame.setVisibleAndInstanceCounts(0,0);
                return;
            }
            lastReuseFlags=NativeRenderer.REUSE_VISIBLE;
            frame.begin(lastInstCount,0,0,indexUploads.size(),0);
            frame.terrain(albedo,lightmap,capture.anisotropy);
            frame.versions(visibleVersion,originsVersion,instancesVersion);
            emitIndexUploads(frame);
            if(lastInstCount>0) {
                MemorySegment.copy(instScratch,0,frame.instanceSegment(),JAVA_INT,0,lastInstCount*4);
            }
            frame.setVisibleAndInstanceCounts(0,lastInstCount);
            return;
        }

        if(capture.fastReused) TerrainCapture.clearDirtyIndices();
        final int count=capture.count;
        final var payloads=capture.payloads;
        final var indicesArr=capture.indices;
        final var originX=capture.originX;
        final var originY=capture.originY;
        final var originZ=capture.originZ;
        final var visibility=capture.visibility;
        final var elementMask=capture.elementMask;
        final var distKey=capture.distKey;
        long total=0;
        // A renderer epoch change or a second preparation of the same snapshot needs a fresh scan.
        boolean checkedAtCapture=capture.checkedResidencyEpoch==epoch && epoch!=0 &&
            (!capture.partialRefresh || (lastFrameCommitted && !hasDeferredUploads));
        capture.checkedResidencyEpoch=0;
        boolean refreshIndices=capture.fastReused || (capture.partialRefresh && !checkedAtCapture);
        int updateCount=checkedAtCapture?capture.uploadCandidates.size():count;
        for(int entry=0;entry<updateCount;entry++) {
            int i=checkedAtCapture?capture.uploadCandidates.getInt(entry):entry;
            var p=payloads[i];
            var indices=indicesArr[i];
            if(refreshIndices || (indices!=null && indices!=p.uploadedIndices && !indices.hasData())) {
                var tr=p.layers[2];
                indices=tr==null?null:tr.indices;
                indicesArr[i]=indices;
            }
            if(p.uploadedEpoch==epoch) {
                if(indices!=null && indices!=p.uploadedIndices && indices.hasData()) {
                    indexUploads.add(new IndexUpload(p,indices)); uploadBytes+=indices.byteSize();
                }
            } else if(p.epoch==capture.epoch && p.hasVertices() && (p.layers[2]==null || (indices!=null && indices.hasData()))) {
                candidates.add(i); total+=p.uploadBytes(indices);
            }
        }
        if(total+uploadBytes>UPLOAD_BUDGET) {
            if(!capture.hasExplicitDistKeys) {
                final double camX=capture.camX,camY=capture.camY,camZ=capture.camZ;
                for(int i=0;i<candidates.size();i++) {
                    int slot=candidates.getInt(i);
                    double dx=originX[slot]-camX,dy=originY[slot]-camY,dz=originZ[slot]-camZ;
                    distKey[slot]=TerrainCapture.distanceKey(dx*dx+dy*dy+dz*dz);
                }
            }
            IntArrays.unstableSort(candidates.elements(),0,candidates.size(),
                (a,b)->Integer.compare(distKey[a],distKey[b]));
        }
        int meshes=0;
        long dataBytes=0;
        for(int i=0;i<candidates.size();i++) {
            int entry=candidates.getInt(i); var p=payloads[entry]; var indices=indicesArr[entry];
            long bytes=p.uploadBytes(indices);
            if(uploadBytes+bytes>UPLOAD_BUDGET && (!staged.isEmpty() || uploadBytes!=0)) break;
            staged.add(entry); p.stagedFrame=frameId;
            p.sectionKey=SectionPos.asLong(originX[entry]>>4,originY[entry]>>4,originZ[entry]>>4);
            uploadBytes+=bytes;
            if(indices!=null) indexUploads.add(new IndexUpload(p,indices));
            for(var layer:p.layers) if(layer!=null) {
                layer.slotId=allocateSlot();
                pendingSlotAllocs.add(layer);
                meshes++;
                if(layer.originalQuadGroups!=null) {
                    dataBytes+=NativeFrameBuffer.alignedBytes(layer.originalQuadGroups.length);
                }
            }
        }
        hasDeferredUploads=staged.size()<candidates.size();
        frame.begin(count*3,pendingRemovals.size()*3,meshes,indexUploads.size(),dataBytes);
        frame.terrain(albedo,lightmap,capture.anisotropy);
        emitIndexUploads(frame);
        for(int i=0;i<staged.size();i++) {
            for(var layer:payloads[staged.getInt(i)].layers) if(layer!=null) {
                frame.meshPooled(Integer.toUnsignedLong(layer.slotId),layer);
            }
        }
        int[] previousInstances=instScratch;
        int[] previousVisible=visScratch;
        ensureScratchCapacity(count);
        boolean originsChanged=previousInstances!=instScratch || originsVersion==0;
        boolean instancesChanged=previousInstances!=instScratch || instancesVersion==0;
        boolean visibleChanged=previousVisible!=visScratch || visibleVersion==0 || !staged.isEmpty();
        final int[] instArr=instScratch;
        final int[] visArr=visScratch;
        final int[] secToInst=sectionToInstance;
        int visCount=0,instCount=0;
        int visOff=0,instOff=0;
        for(int i=0;i<count;i++) {
            var p=payloads[i]; int mask=elementMask[i];
            if(p.uploadedEpoch!=epoch && p.stagedFrame!=frameId) {
                long key=SectionPos.asLong(originX[i]>>4,originY[i]>>4,originZ[i]>>4);
                p=residentBySection.get(key);
                if(p==null || p.uploadedEpoch!=epoch) { secToInst[i]=-1; continue; }
                mask=0;
                var layers=p.layers;
                if(layers[0]!=null) mask|=1;
                if(layers[1]!=null) mask|=1<<8;
                if(layers[2]!=null) mask|=1<<16;
            }
            p.lastActiveFrame=frameId;
            if(mask==0) { secToInst[i]=-1; continue; }
            int instance=instCount++;
            secToInst[i]=instance;
            int ox=originX[i], oy=originY[i], oz=originZ[i];
            int visBits=Float.floatToRawIntBits(visibility[i]);
            boolean originDiff=instArr[instOff]!=ox || instArr[instOff+1]!=oy || instArr[instOff+2]!=oz;
            originsChanged|=originDiff;
            instancesChanged|=originDiff || instArr[instOff+3]!=visBits;
            instArr[instOff]=ox;
            instArr[instOff+1]=oy;
            instArr[instOff+2]=oz;
            instArr[instOff+3]=visBits;
            instOff+=4;
            var layers=p.layers;
            var l0=layers[0];
            if(l0!=null && (mask&127)!=0) {
                int slotId=l0.slotId;
                visibleChanged|=visArr[visOff]!=slotId || visArr[visOff+1]!=0 || visArr[visOff+2]!=-1 ||
                    visArr[visOff+3]!=0 || visArr[visOff+4]!=instance || visArr[visOff+5]!=1;
                visArr[visOff]=slotId;
                visArr[visOff+1]=0;
                visArr[visOff+2]=-1;
                visArr[visOff+3]=0;
                visArr[visOff+4]=instance;
                visArr[visOff+5]=1;
                visOff+=6; visCount++;
            }
            var l1=layers[1];
            if(l1!=null && ((mask>>>8)&127)!=0) {
                int slotId=l1.slotId;
                visibleChanged|=visArr[visOff]!=slotId || visArr[visOff+1]!=0 || visArr[visOff+2]!=-1 ||
                    visArr[visOff+3]!=0 || visArr[visOff+4]!=instance || visArr[visOff+5]!=1;
                visArr[visOff]=slotId;
                visArr[visOff+1]=0;
                visArr[visOff+2]=-1;
                visArr[visOff+3]=0;
                visArr[visOff+4]=instance;
                visArr[visOff+5]=1;
                visOff+=6; visCount++;
            }
            var l2=layers[2];
            if(l2!=null && ((mask>>>16)&127)!=0) {
                int slotId=l2.slotId;
                int sortKey=count-i;
                visibleChanged|=visArr[visOff]!=slotId || visArr[visOff+1]!=0 || visArr[visOff+2]!=-1 ||
                    visArr[visOff+3]!=sortKey || visArr[visOff+4]!=instance || visArr[visOff+5]!=1;
                visArr[visOff]=slotId;
                visArr[visOff+1]=0;
                visArr[visOff+2]=-1;
                visArr[visOff+3]=sortKey;
                visArr[visOff+4]=instance;
                visArr[visOff+5]=1;
                visOff+=6; visCount++;
            }
        }
        for(int i=pendingRemovals.size()-1;i>=0;i--) {
            var p=pendingRemovals.get(i);
            if(p.uploadedEpoch!=epoch) {
                if(p.stagedFrame!=frameId) {
                    p.releaseBuffers();
                    swapPop(pendingRemovals,i);
                }
                continue;
            }
            if(p.lastActiveFrame==frameId) continue;
            evicting.add(p);
            for(var layer:p.layers) if(layer!=null) {
                frame.removeMesh(Integer.toUnsignedLong(layer.slotId));
            }
        }
        if(instCount>0) MemorySegment.copy(instArr,0,frame.instanceSegment(),JAVA_INT,0,instOff);
        if(visCount>0) MemorySegment.copy(visArr,0,frame.visibleSegment(),JAVA_INT,0,visOff);
        frame.setVisibleAndInstanceCounts(visCount,instCount);
        if(visibleChanged || lastVisCount!=visCount || !evicting.isEmpty()) visibleVersion=frameId;
        if(originsChanged || lastInstCount!=instCount) originsVersion=frameId;
        if(instancesChanged || lastInstCount!=instCount) instancesVersion=frameId;
        frame.versions(visibleVersion,originsVersion,instancesVersion);
        lastVisCount=visCount;
        lastInstCount=instCount;
    }
    record ViewTransition(Runnable submitted,Runnable rollback) {}
    void commit() { stageCommit().submitted().run(); }
    ViewTransition stageCommit() {
        var preparedCapture=capture;
        long preparedEpoch=epoch;
        var undo=new ObjectArrayList<Runnable>();
        if(!pendingSlotAllocs.isEmpty()) {
            var allocated=new ObjectArrayList<>(pendingSlotAllocs);
            undo.add(()->{
                for(int i=allocated.size()-1;i>=0;--i) {
                    var layer=allocated.get(i);
                    freeSlots.push(layer.slotId);
                    layer.slotId=0;
                }
            });
        }
        uncommittedPrepare=false;
        lastFrameCommitted=pendingAllowReuse;
        lastInstancesCommitted=pendingAllowReuse;
        pendingSlotAllocs.clear();
        ObjectArrayList<MeshPayload> uploaded=staged.isEmpty()?null:new ObjectArrayList<>(staged.size()*2);
        for(int i=0;i<staged.size();i++) {
            var p=capture.payloads[staged.getInt(i)];
            long previousEpoch=p.uploadedEpoch;
            var previous=residentBySection.put(p.sectionKey,p);
            p.uploadedEpoch=epoch; residentCount++;
            undo.add(()->{
                p.uploadedEpoch=previousEpoch;
                if(previous==null) residentBySection.remove(p.sectionKey); else residentBySection.put(p.sectionKey,previous);
                --residentCount;
            });
            for(var layer:p.layers) if(layer!=null) uploaded.add(layer);
        }
        ObjectArrayList<MeshPayload.IndexSnapshot> uploadedIdx=indexUploads.isEmpty()?null:new ObjectArrayList<>(indexUploads.size());
        for(int i=0;i<indexUploads.size();i++) {
            var update=indexUploads.get(i);
            var previous=update.section.uploadedIndices;
            undo.add(()->{ update.section.uploadedIndices=previous; TerrainCapture.recordDirtyIndices(update.section); });
            update.section.uploadedIndices=update.snapshot;
            uploadedIdx.add(update.snapshot);
        }
        ObjectArrayList<SectionPayload> evictedCopy=evicting.isEmpty()?null:new ObjectArrayList<>(evicting);
        for(int i=0;i<evicting.size();i++) {
            var p=evicting.get(i);
            var previousIndices=p.uploadedIndices;
            var previousResident=residentBySection.get(p.sectionKey);
            int[] slots=new int[p.layers.length];
            for(int j=0;j<slots.length;++j) if(p.layers[j]!=null) slots[j]=p.layers[j].slotId;
            undo.add(()->{
                p.uploadedEpoch=preparedEpoch; p.uploadedIndices=previousIndices;
                for(int j=slots.length-1;j>=0;--j) if(slots[j]!=0) {
                    if(!freeSlots.rem(slots[j])) throw new IllegalStateException("Evicted terrain slot is no longer free");
                    p.layers[j].slotId=slots[j];
                }
                if(previousResident!=null) residentBySection.put(p.sectionKey,previousResident);
                ++residentCount;
                if(!pendingRemovals.contains(p)) pendingRemovals.add(p);
            });
            p.uploadedEpoch=0; p.uploadedIndices=null;
            for(var layer:p.layers) if(layer!=null && layer.slotId!=0) {
                freeSlots.push(layer.slotId);
                layer.slotId=0;
            }
            if(residentBySection.get(p.sectionKey)==p) residentBySection.remove(p.sectionKey);
            residentCount--;
        }
        if(!evicting.isEmpty()) {
            for(int i=pendingRemovals.size()-1;i>=0;i--) {
                if(pendingRemovals.get(i).uploadedEpoch!=epoch) swapPop(pendingRemovals,i);
            }
        }
        Runnable rollback=()->{
            if(epoch!=preparedEpoch) return;
            for(int i=undo.size()-1;i>=0;--i) undo.get(i).run();
            lastFrameCommitted=lastInstancesCommitted=false;
            if(preparedCapture!=null) preparedCapture.checkedResidencyEpoch=0;
        };
        if((uploaded==null || uploaded.isEmpty()) && (uploadedIdx==null || uploadedIdx.isEmpty()) && evictedCopy==null)
            return new ViewTransition(NOOP,rollback);
        final var toClear=uploaded;
        final var toClearIdx=uploadedIdx;
        final var toClearEvicted=evictedCopy;
        return new ViewTransition(()->{
            if(toClear!=null) for(int i=0;i<toClear.size();i++) toClear.get(i).releaseVertices();
            if(toClearIdx!=null) for(int i=0;i<toClearIdx.size();i++) toClearIdx.get(i).releaseBuffer();
            if(toClearEvicted!=null) for(int i=0;i<toClearEvicted.size();i++) toClearEvicted.get(i).releaseBuffers();
        },rollback);
    }
    int residentCount() { return residentCount; }
}
