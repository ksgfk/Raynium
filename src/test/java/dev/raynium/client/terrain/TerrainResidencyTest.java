package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeFrameBuffer;
import dev.raynium.bridge.NativeRenderer;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import org.junit.jupiter.api.Test;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

class TerrainResidencyTest {
    private static SectionPayload mesh(int bytes) { return new SectionPayload(1,new MeshPayload(1,ByteBuffer.allocate(Math.max(112,bytes/112*112)))); }
    private static TerrainCapture.Buffer capture() { var c=new TerrainCapture.Buffer(); c.anisotropy=1; return c; }
    private static void put(TerrainCapture.Buffer c,int slot,SectionPayload p,int x,int distance,int mask) {
        c.put(slot,p,x,0,0,1,mask,TerrainCapture.distanceKey(distance)); c.finish(slot+1);
    }
    private static MemorySegment descriptor(NativeFrameBuffer frame) { return frame.finish(new NativeRenderer.Frame()); }
    private static int count(NativeFrameBuffer frame,int array) { return descriptor(frame).get(JAVA_INT,112+array*4L); }
    private static long id(NativeFrameBuffer frame,int array,int row,int stride) {
        return descriptor(frame).get(ADDRESS,32+array*8L).reinterpret((row+1L)*stride).get(JAVA_LONG,row*(long)stride);
    }
    @Test void overBudgetSelectsNearestAndAllowsOneOversizedMesh() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var far=mesh(5*1024*1024); var near=mesh(5*1024*1024); var c=capture();
            put(c,0,far,16,100,1); put(c,1,near,0,1,1);
            residency.prepare(c,frame); assertEquals(1,count(frame,1)); assertEquals(near.layers[0].id,id(frame,1,0,40));
            assertEquals(0,near.uploadedEpoch); residency.commit();
            assertEquals(epoch,near.uploadedEpoch); assertEquals(0,far.uploadedEpoch);
            residency.prepare(c,frame); residency.commit(); assertEquals(epoch,far.uploadedEpoch);
            var huge=mesh(11_200_000); c.clear(); put(c,0,huge,32,1,1);
            residency.prepare(c,frame); assertEquals(1,count(frame,1)); assertEquals(huge.layers[0].id,id(frame,1,0,40));
            assertTrue(residency.uploadBytes>TerrainResidency.UPLOAD_BUDGET); residency.commit();
            assertEquals(epoch,huge.uploadedEpoch);
        }
    }
    @Test void closedFallbackSurvivesMultipleFramesThenEvictsAfterReplacement() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var old=mesh(112); var c=capture(); put(c,0,old,0,100,1);
            residency.prepare(c,frame); residency.commit(); residency.pendingRemovals.add(old);
            var replacement=mesh(5*1024*1024);
            for(int i=0;i<2;i++) {
                c.clear(); put(c,0,replacement,0,100,1); put(c,1,mesh(9*1024*1024),16+i*16,1,1);
                residency.prepare(c,frame);
                assertEquals(old.layers[0].id,id(frame,8,0,24)); assertEquals(0,count(frame,4));
                residency.commit(); assertEquals(epoch,old.uploadedEpoch); assertTrue(residency.pendingRemovals.contains(old));
            }
            residency.prepare(c,frame);
            assertEquals(replacement.layers[0].id,id(frame,8,0,24)); assertEquals(old.layers[0].id,id(frame,4,0,8));
            assertEquals(epoch,old.uploadedEpoch); assertEquals(0,replacement.uploadedEpoch);
            residency.commit(); assertEquals(0,old.uploadedEpoch); assertEquals(epoch,replacement.uploadedEpoch);
            assertTrue(residency.pendingRemovals.isEmpty()); assertEquals(3,residency.residentCount());
            // The old removal must not erase the new occupant of the same section.
            residency.pendingRemovals.add(replacement); c.clear();
            var next=mesh(5*1024*1024); put(c,0,next,0,100,1); put(c,1,mesh(9*1024*1024),64,1,1);
            residency.prepare(c,frame); assertEquals(replacement.layers[0].id,id(frame,8,0,24));
            c.clear(); residency.prepare(c,frame);
            assertEquals(replacement.layers[0].id,id(frame,4,0,8)); residency.commit(); assertEquals(0,replacement.uploadedEpoch);
        }
    }
    @Test void zeroMaskIsActiveWithoutInstancesAndLeavingViewReclaimsClosedMesh() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId());
            var p=mesh(112); var c=capture(); put(c,0,p,0,1,0);
            residency.prepare(c,frame); residency.commit(); residency.pendingRemovals.add(p);
            residency.prepare(c,frame); assertEquals(0,count(frame,8)); assertEquals(0,count(frame,9)); assertEquals(0,count(frame,4));
            residency.commit(); assertEquals(1,residency.pendingRemovals.size());
            c.clear(); residency.prepare(c,frame); assertEquals(p.layers[0].id,id(frame,4,0,8)); residency.commit();
            assertEquals(0,p.uploadedEpoch); assertEquals(0,residency.residentCount()); assertTrue(residency.pendingRemovals.isEmpty());
        }
    }
    @Test void uncommittedUploadAndEvictionDoNotChangeResidencyAndNewOwnerCanReupload() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var p=mesh(112); var vertices=p.layers[0].vertices; var c=capture(); put(c,0,p,0,1,1);
            residency.prepare(c,frame); assertEquals(0,p.uploadedEpoch); assertEquals(0,residency.residentCount());
            residency.prepare(c,frame); assertEquals(1,count(frame,1)); residency.commit();
            residency.pendingRemovals.add(p); c.clear(); residency.prepare(c,frame);
            assertEquals(epoch,p.uploadedEpoch); assertEquals(1,residency.pendingRemovals.size());
            long nextEpoch=NativeRenderer.nextFrameId(); assertTrue(nextEpoch>p.lastActiveFrame);
            residency.reset(nextEpoch); put(c,0,p,0,1,1); residency.prepare(c,frame);
            assertEquals(1,count(frame,1)); assertSame(vertices,p.layers[0].vertices); residency.commit();
            assertEquals(nextEpoch,p.uploadedEpoch); assertEquals(1,residency.residentCount());
        }
    }
    @Test void closeBetweenCaptureAndFirstUploadIsRetainedUntilTheFollowingFrame() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId());
            var p=mesh(112); var c=capture(); put(c,0,p,0,1,1);
            residency.pendingRemovals.add(p); residency.prepare(c,frame); residency.commit();
            assertEquals(1,residency.pendingRemovals.size()); assertEquals(1,residency.residentCount());
            c.clear(); residency.prepare(c,frame); assertEquals(p.layers[0].id,id(frame,4,0,8)); residency.commit();
            assertEquals(0,residency.residentCount()); assertTrue(residency.pendingRemovals.isEmpty());
        }
    }
    @Test void threeLayersAreAdoptedTogetherAndTransparentIndicesAreFrozen() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var solid=new MeshPayload(1,ByteBuffer.allocate(112),0);
            var cutout=new MeshPayload(1,ByteBuffer.allocate(112),1);
            var translucent=new MeshPayload(1,ByteBuffer.allocate(112),2);
            var first=new MeshPayload.IndexSnapshot(new int[]{0,1,2,2,3,0}); translucent.indices=first;
            var section=new SectionPayload(1,solid,cutout,translucent); var c=capture();
            put(c,0,section,0,1,0x010101);
            var later=new MeshPayload.IndexSnapshot(new int[]{3,2,1,1,0,3}); translucent.indices=later;
            residency.prepare(c,frame);
            assertEquals(3,count(frame,1)); assertEquals(3,count(frame,8)); assertEquals(0,section.uploadedEpoch);
            residency.commit(); assertSame(first,section.uploadedIndices); assertEquals(epoch,section.uploadedEpoch);
            // A new snapshot is not committed until native submission succeeds; vertex uploads stay zero.
            put(c,0,section,0,1,0x010101); residency.prepare(c,frame);
            assertEquals(0,count(frame,1)); assertEquals(1,count(frame,0)); assertSame(first,section.uploadedIndices);
            residency.commit(); assertSame(later,section.uploadedIndices);
            residency.prepare(c,frame); assertEquals(0,count(frame,0)); assertEquals(0,residency.uploadBytes);
            residency.pendingRemovals.add(section); c.clear(); residency.prepare(c,frame);
            assertEquals(3,count(frame,4)); assertEquals(1,count(frame,7)); residency.commit(); assertEquals(0,residency.residentCount());
        }
    }
    @Test void replacementWithoutWaterDoesNotRetainOldTransparentLayer() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId()); var c=capture();
            var water=new MeshPayload(1,ByteBuffer.allocate(112),2);
            water.indices=new MeshPayload.IndexSnapshot(new int[]{0,1,2,2,3,0});
            var old=new SectionPayload(1,water); put(c,0,old,0,1,0x010000);
            residency.prepare(c,frame); residency.commit(); residency.pendingRemovals.add(old);
            var next=mesh(112); c.clear(); put(c,0,next,0,1,1); residency.prepare(c,frame);
            assertEquals(1,count(frame,8)); assertEquals(next.layers[0].id,id(frame,8,0,24));
            assertEquals(water.id,id(frame,5,0,8)); assertEquals(water.id+1,id(frame,7,0,8)); residency.commit();
            assertEquals(1,residency.residentCount());
        }
    }

}
