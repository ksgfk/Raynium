package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeFrameBuffer;
import dev.raynium.bridge.NativeRenderer;
import it.unimi.dsi.fastutil.ints.IntArrayList;
import it.unimi.dsi.fastutil.ints.IntArrays;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import it.unimi.dsi.fastutil.objects.ObjectArrayList;
import net.minecraft.core.SectionPos;

/** Render-thread residency. Commit only after a successful native submission. */
final class TerrainResidency {
    static final long UPLOAD_BUDGET=8L*1024*1024;
    private final Long2ObjectOpenHashMap<MeshPayload> residentBySection=new Long2ObjectOpenHashMap<>(4096);
    final ObjectArrayList<MeshPayload> pendingRemovals=new ObjectArrayList<>();
    private final IntArrayList unuploadedIndices=new IntArrayList(1024);
    private final ObjectArrayList<MeshPayload> stagedMeshes=new ObjectArrayList<>(),evictingPayloads=new ObjectArrayList<>();
    private long epoch,frameId;
    private int indexCapacity,anisotropy,nextIndexCapacity,nextAnisotropy,residentCount;
    long uploadBytes;

    void reset(long epoch) {
        residentBySection.clear(); pendingRemovals.clear(); stagedMeshes.clear(); evictingPayloads.clear(); unuploadedIndices.clear();
        this.epoch=epoch; indexCapacity=anisotropy=residentCount=0;
    }
    void prepare(TerrainCapture.Buffer capture,NativeFrameBuffer frame) {
        frameId=NativeRenderer.nextFrameId();
        unuploadedIndices.clear(); stagedMeshes.clear(); evictingPayloads.clear();
        long total=0;
        for(int i=0;i<capture.count;i++) {
            var p=capture.payloads[i];
            if(p.uploadedEpoch!=epoch && p.stagedFrame!=frameId && p.vertices.length>0) {
                p.stagedFrame=frameId;
                p.sectionKey=SectionPos.asLong(capture.originX[i]>>4,capture.originY[i]>>4,capture.originZ[i]>>4);
                unuploadedIndices.add(i); total+=p.vertices.length;
            }
        }
        if(total>UPLOAD_BUDGET) {
            for(int i=0;i<unuploadedIndices.size();i++) capture.payloads[unuploadedIndices.getInt(i)].stagedFrame=0;
            IntArrays.unstableSort(unuploadedIndices.elements(),0,unuploadedIndices.size(),
                (a,b)->Integer.compare(capture.distKey[a],capture.distKey[b]));
        }
        uploadBytes=0; long dataBytes=0; int needed=indexCapacity,ranges=0;
        for(int i=0;i<unuploadedIndices.size();i++) {
            var p=capture.payloads[unuploadedIndices.getInt(i)];
            if(total>UPLOAD_BUDGET && uploadBytes!=0 && uploadBytes+p.vertices.length>UPLOAD_BUDGET) break;
            p.stagedFrame=frameId; stagedMeshes.add(p); uploadBytes+=p.vertices.length;
            dataBytes+=NativeFrameBuffer.alignedBytes(p.vertices.length);
            needed=Math.max(needed,p.vertices.length/28); ranges+=Integer.bitCount(p.allElementMask);
        }
        nextIndexCapacity=needed>indexCapacity?Math.max(65536,Integer.highestOneBit(needed-1)<<1):indexCapacity;
        nextAnisotropy=capture.anisotropy;
        boolean indexChanged=nextIndexCapacity!=indexCapacity,materialChanged=anisotropy!=nextAnisotropy;
        if(indexChanged) dataBytes+=NativeFrameBuffer.alignedBytes(nextIndexCapacity/4*6L*4);
        frame.begin(capture.count,pendingRemovals.size(),stagedMeshes.size(),ranges,materialChanged?1:0,indexChanged?1:0,dataBytes);
        if(indexChanged) frame.quadIndices(1,nextIndexCapacity);
        if(materialChanged) frame.material(1,0,1,nextAnisotropy);
        for(int i=0;i<stagedMeshes.size();i++) {
            var p=stagedMeshes.get(i);
            frame.mesh(p.id,p.vertices,1,p.vertices.length/28,Integer.bitCount(p.allElementMask));
            int first=0;
            for(int count:p.groupVertices) {
                int indices=count/4*6;
                if(indices>0) frame.submesh(first,indices,0);
                first+=indices;
            }
            frame.primitive(p.id,p.id,1);
        }
        for(int i=0;i<capture.count;i++) {
            var p=capture.payloads[i]; int mask=capture.elementMask[i];
            if(p.uploadedEpoch!=epoch && p.stagedFrame!=frameId) {
                long key=SectionPos.asLong(capture.originX[i]>>4,capture.originY[i]>>4,capture.originZ[i]>>4);
                p=residentBySection.get(key);
                if(p==null || p.uploadedEpoch!=epoch) continue;
                mask=p.allElementMask;
            }
            p.lastActiveFrame=frameId;
            if(mask==0) continue;
            int instance=frame.instance(capture.originX[i],capture.originY[i],capture.originZ[i],capture.visibility[i]);
            frame.visible(p.id,mask,capture.distKey[i]>>>16,instance,1);
        }
        for(int i=pendingRemovals.size()-1;i>=0;i--) {
            var p=pendingRemovals.get(i);
            if(p.uploadedEpoch!=epoch) {
                // A frozen snapshot can be closed before its first native upload completes.
                if(p.stagedFrame!=frameId) pendingRemovals.remove(i);
                continue;
            }
            if(p.lastActiveFrame==frameId) continue;
            evictingPayloads.add(p); frame.removeMesh(p.id); frame.removePrimitive(p.id);
        }
    }
    void commit() {
        indexCapacity=nextIndexCapacity; anisotropy=nextAnisotropy;
        for(int i=0;i<stagedMeshes.size();i++) {
            var p=stagedMeshes.get(i); p.uploadedEpoch=epoch; residentBySection.put(p.sectionKey,p); residentCount++;
        }
        for(int i=0;i<evictingPayloads.size();i++) {
            var p=evictingPayloads.get(i); p.uploadedEpoch=0;
            if(residentBySection.get(p.sectionKey)==p) residentBySection.remove(p.sectionKey);
            residentCount--;
        }
        if(!evictingPayloads.isEmpty()) {
            for(int i=pendingRemovals.size()-1;i>=0;i--) if(pendingRemovals.get(i).uploadedEpoch!=epoch) pendingRemovals.remove(i);
        }
    }
    int residentCount() { return residentCount; }
}
