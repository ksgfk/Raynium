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
    private int residentCount;
    long uploadBytes;

    void reset(long epoch) {
        residentBySection.clear(); pendingRemovals.clear(); staged.clear(); evicting.clear(); candidates.clear(); indexUploads.clear();
        capture=null; this.epoch=epoch; residentCount=0;
    }
    void prepare(TerrainCapture.Buffer capture,NativeFrameBuffer frame,int albedo,int lightmap) {
        this.capture=capture; frameId=NativeRenderer.nextFrameId();
        candidates.clear(); staged.clear(); evicting.clear(); indexUploads.clear();
        long total=0; uploadBytes=0;
        for(int i=0;i<capture.count;i++) {
            var p=capture.payloads[i]; var indices=capture.indices[i];
            if(p.uploadedEpoch==epoch) {
                if(indices!=null && indices!=p.uploadedIndices) {
                    indexUploads.add(new IndexUpload(p,indices)); uploadBytes+=indices.values().length*4L;
                }
            } else if(p.epoch==capture.epoch && p.vertexBytes>0 && (p.layers[2]==null || indices!=null)) {
                candidates.add(i); total+=p.uploadBytes(indices);
            }
        }
        if(total+uploadBytes>UPLOAD_BUDGET) IntArrays.unstableSort(candidates.elements(),0,candidates.size(),
            (a,b)->Integer.compare(capture.distKey[a],capture.distKey[b]));
        int meshes=0;
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
                meshes++;
                dataBytes+=NativeFrameBuffer.alignedBytes(layer.vertices.length);
            }
        }
        for(var update:indexUploads) dataBytes+=NativeFrameBuffer.alignedBytes(update.snapshot.values().length*4L);
        frame.begin(capture.count*3,pendingRemovals.size()*3,meshes,indexUploads.size(),dataBytes);
        frame.terrain(albedo,lightmap,capture.anisotropy);
        for(var update:indexUploads) frame.index(update.section.layers[2].id,update.snapshot.values());
        for(int entry:staged) for(var layer:capture.payloads[entry].layers) if(layer!=null)
            frame.mesh(layer.id,layer.vertices,layer.vertexCount,layer.mode);
        for(int i=0;i<capture.count;i++) {
            var p=capture.payloads[i]; int mask=capture.elementMask[i];
            if(p.uploadedEpoch!=epoch && p.stagedFrame!=frameId) {
                long key=SectionPos.asLong(capture.originX[i]>>4,capture.originY[i]>>4,capture.originZ[i]>>4);
                p=residentBySection.get(key);
                if(p==null || p.uploadedEpoch!=epoch) continue;
                mask=0;
                for(int layer=0;layer<3;layer++) if(p.layers[layer]!=null) mask|=1<<(layer*8);
            }
            p.lastActiveFrame=frameId;
            if(mask==0) continue;
            int instance=frame.instance(capture.originX[i],capture.originY[i],capture.originZ[i],capture.visibility[i]);
            for(int layer=0;layer<3;layer++) if(p.layers[layer]!=null) {
                int elements=(mask>>>(layer*8))&127;
                if(elements!=0) frame.visible(p.layers[layer].id,-1,layer==2?capture.count-i:capture.distKey[i]>>>16,instance,1);
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
                frame.removeMesh(layer.id);
            }
        }
    }
    void commit() {
        for(int entry:staged) {
            var p=capture.payloads[entry]; p.uploadedEpoch=epoch; residentBySection.put(p.sectionKey,p); residentCount++;
            for(var layer:p.layers) if(layer!=null) layer.vertices=null;
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
