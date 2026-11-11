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
    private final Long2ObjectOpenHashMap<SectionPayload> residentBySection=new Long2ObjectOpenHashMap<>(4096);
    final ObjectArrayList<SectionPayload> pendingRemovals=new ObjectArrayList<>();
    private final IntArrayList candidates=new IntArrayList(1024),staged=new IntArrayList(1024);
    private final ObjectArrayList<SectionPayload> evicting=new ObjectArrayList<>();
    private record IndexUpload(SectionPayload section,MeshPayload.IndexSnapshot snapshot) {}
    private final ObjectArrayList<IndexUpload> indexUploads=new ObjectArrayList<>();
    private TerrainCapture.Buffer capture;
    private long epoch,frameId;
    private int indexCapacity,anisotropy,nextIndexCapacity,nextAnisotropy,residentCount;
    long uploadBytes;

    void reset(long epoch) {
        residentBySection.clear(); pendingRemovals.clear(); staged.clear(); evicting.clear(); candidates.clear(); indexUploads.clear();
        capture=null; this.epoch=epoch; indexCapacity=anisotropy=residentCount=0;
    }
    void prepare(TerrainCapture.Buffer capture,NativeFrameBuffer frame) {
        this.capture=capture; frameId=NativeRenderer.nextFrameId();
        candidates.clear(); staged.clear(); evicting.clear(); indexUploads.clear();
        long total=0; uploadBytes=0;
        for(int i=0;i<capture.count;i++) {
            var p=capture.payloads[i]; var indices=capture.indices[i];
            if(p.uploadedEpoch==epoch) {
                if(indices!=null && indices!=p.uploadedIndices) {
                    indexUploads.add(new IndexUpload(p,indices)); uploadBytes+=indices.values().length*4L;
                }
            } else if(p.vertexBytes>0 && (p.layers[2]==null || indices!=null)) {
                candidates.add(i); total+=p.uploadBytes(indices);
            }
        }
        if(total+uploadBytes>UPLOAD_BUDGET) IntArrays.unstableSort(candidates.elements(),0,candidates.size(),
            (a,b)->Integer.compare(capture.distKey[a],capture.distKey[b]));
        int needed=indexCapacity,meshes=0,ranges=0;
        long dataBytes=0;
        for(int i=0;i<candidates.size();i++) {
            int entry=candidates.getInt(i); var p=capture.payloads[entry]; var indices=capture.indices[entry];
            long bytes=p.uploadBytes(indices);
            if(uploadBytes+bytes>UPLOAD_BUDGET && (!staged.isEmpty() || uploadBytes!=0)) break;
            staged.add(entry); p.stagedFrame=frameId;
            p.sectionKey=SectionPos.asLong(capture.originX[entry]>>4,capture.originY[entry]>>4,capture.originZ[entry]>>4);
            uploadBytes+=bytes;
            if(indices!=null) indexUploads.add(new IndexUpload(p,indices));
            for(var layer:p.layers) if(layer!=null) {
                meshes++; ranges+=Integer.bitCount(layer.allElementMask);
                dataBytes+=NativeFrameBuffer.alignedBytes(layer.vertices.length);
                if(layer.mode!=2) needed=Math.max(needed,layer.vertices.length/28);
            }
        }
        for(var update:indexUploads) dataBytes+=NativeFrameBuffer.alignedBytes(update.snapshot.values().length*4L);
        nextIndexCapacity=needed>indexCapacity?Math.max(65536,Integer.highestOneBit(needed-1)<<1):indexCapacity;
        nextAnisotropy=capture.anisotropy;
        boolean indexChanged=nextIndexCapacity!=indexCapacity,materialChanged=anisotropy!=nextAnisotropy;
        if(indexChanged) dataBytes+=NativeFrameBuffer.alignedBytes(nextIndexCapacity/4*6L*4);
        frame.begin(capture.count*3,pendingRemovals.size()*3,meshes,ranges,materialChanged?3:0,
            indexUploads.size()+(indexChanged?1:0),dataBytes);
        if(indexChanged) frame.quadIndices(1,nextIndexCapacity);
        if(materialChanged) for(int mode=0;mode<3;mode++) frame.material(mode+1,0,1,nextAnisotropy,mode);
        for(var update:indexUploads) frame.index(update.section.layers[2].id+1,update.snapshot.values());
        for(int entry:staged) for(var layer:capture.payloads[entry].layers) if(layer!=null) {
            frame.mesh(layer.id,layer.vertices,layer.mode==2?layer.id+1:1,layer.vertices.length/28,Integer.bitCount(layer.allElementMask));
            int first=0;
            for(int count:layer.groupVertices) {
                int indices=count/4*6;
                if(indices>0) frame.submesh(first,indices,0);
                first+=indices;
            }
            frame.primitive(layer.id,layer.id,layer.mode+1);
        }
        for(int i=0;i<capture.count;i++) {
            var p=capture.payloads[i]; int mask=capture.elementMask[i];
            if(p.uploadedEpoch!=epoch && p.stagedFrame!=frameId) {
                long key=SectionPos.asLong(capture.originX[i]>>4,capture.originY[i]>>4,capture.originZ[i]>>4);
                p=residentBySection.get(key);
                if(p==null || p.uploadedEpoch!=epoch) continue;
                mask=0;
                for(int layer=0;layer<3;layer++) if(p.layers[layer]!=null) mask|=p.layers[layer].allElementMask<<(layer*8);
            }
            p.lastActiveFrame=frameId;
            if(mask==0) continue;
            int instance=frame.instance(capture.originX[i],capture.originY[i],capture.originZ[i],capture.visibility[i]);
            for(int layer=0;layer<3;layer++) if(p.layers[layer]!=null) {
                int elements=(mask>>>(layer*8))&127;
                if(elements!=0) frame.visible(p.layers[layer].id,elements,layer==2?capture.count-i:capture.distKey[i]>>>16,instance,1);
            }
        }
        for(int i=pendingRemovals.size()-1;i>=0;i--) {
            var p=pendingRemovals.get(i);
            if(p.uploadedEpoch!=epoch) {
                if(p.stagedFrame!=frameId) pendingRemovals.remove(i);
                continue;
            }
            if(p.lastActiveFrame==frameId) continue;
            evicting.add(p);
            for(var layer:p.layers) if(layer!=null) {
                frame.removeMesh(layer.id); frame.removePrimitive(layer.id);
                if(layer.mode==2) frame.removeIndex(layer.id+1);
            }
        }
    }
    void commit() {
        indexCapacity=nextIndexCapacity; anisotropy=nextAnisotropy;
        for(int entry:staged) {
            var p=capture.payloads[entry]; p.uploadedEpoch=epoch; residentBySection.put(p.sectionKey,p); residentCount++;
        }
        for(var update:indexUploads) update.section.uploadedIndices=update.snapshot;
        for(var p:evicting) {
            p.uploadedEpoch=0; p.uploadedIndices=null;
            if(residentBySection.get(p.sectionKey)==p) residentBySection.remove(p.sectionKey);
            residentCount--;
        }
        if(!evicting.isEmpty()) {
            for(int i=pendingRemovals.size()-1;i>=0;i--) if(pendingRemovals.get(i).uploadedEpoch!=epoch) pendingRemovals.remove(i);
        }
    }
    int residentCount() { return residentCount; }
}
