package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeFrameBuffer;
import dev.raynium.bridge.NativeRenderer;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

class TerrainResidencyTest {
    @AfterEach void cleanup() {
        TerrainCapture.activate(false);
        DirectUploadPool.clearForTests();
    }
    private static SectionPayload mesh(int bytes) { return new SectionPayload(1,new MeshPayload(1,ByteBuffer.allocate(Math.max(112,bytes/112*112)))); }
    private static TerrainCapture.Buffer capture() { var c=new TerrainCapture.Buffer(); c.anisotropy=1; c.epoch=1; return c; }
    private static void put(TerrainCapture.Buffer c,int slot,SectionPayload p,int x,int distance,int mask) {
        c.put(slot,p,x,0,0,0,mask,TerrainCapture.distanceKey(distance)); c.finish(slot+1);
    }
    private static MemorySegment descriptor(NativeFrameBuffer frame) { return frame.finish(new NativeRenderer.Frame()); }
    private static int count(NativeFrameBuffer frame,int array) { return descriptor(frame).get(JAVA_INT,64+array*4L); }
    private static long id(NativeFrameBuffer frame,int array,int row,int stride) {
        return descriptor(frame).get(ADDRESS,24+array*8L).reinterpret((row+1L)*stride).get(JAVA_LONG,row*(long)stride);
    }
    private static byte[] makeQuad(int group,float plane) {
        float[][] pts=new float[4][3];
        if(group<6) {
            int a=group/2,b=(a+1)%3,c=(a+2)%3;
            for(float[] p:pts) p[a]=plane;
            pts[1][b]=1; pts[2][b]=1; pts[2][c]=1; pts[3][c]=1;
            if((group&1)!=0) {
                float[] tmp=pts[1]; pts[1]=pts[3]; pts[3]=tmp;
            }
        }
        ByteBuffer buf=ByteBuffer.allocate(112).order(ByteOrder.nativeOrder());
        for(int v=0;v<4;v++) {
            buf.putFloat(pts[v][0]).putFloat(pts[v][1]).putFloat(pts[v][2]);
            buf.putInt(0x12340000+group);
            buf.putFloat(0f).putFloat(0f).putInt(0);
        }
        return buf.array();
    }
    private static MeshPayload.IndexSnapshot indices(MeshPayload mesh,int... values) {
        var bytes=ByteBuffer.allocate(values.length*4).order(ByteOrder.nativeOrder());
        bytes.asIntBuffer().put(values);
        return mesh.copyIndices(bytes,4);
    }

    @Test void overBudgetSelectsNearestAndAllowsOneOversizedMesh() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var far=mesh(2*1024*1024); var near=mesh(2*1024*1024); var c=capture();
            put(c,0,far,16,100,1); put(c,1,near,0,1,1);
            residency.prepare(c,frame,16,17); assertEquals(1,count(frame,1)); assertEquals(near.layers[0].slotId,id(frame,1,0,32));
            assertEquals(1,near.layers[0].slotId);
            assertEquals(0,near.uploadedEpoch); residency.commit();
            assertEquals(epoch,near.uploadedEpoch); assertEquals(0,far.uploadedEpoch);
            residency.prepare(c,frame,16,17); residency.commit(); assertEquals(epoch,far.uploadedEpoch);
            assertEquals(2,far.layers[0].slotId);
            var huge=mesh(4*1024*1024); c.clear(); put(c,0,huge,32,1,1);
            residency.prepare(c,frame,16,17); assertEquals(1,count(frame,1)); assertEquals(huge.layers[0].slotId,id(frame,1,0,32));
            assertTrue(residency.uploadBytes>TerrainResidency.UPLOAD_BUDGET); residency.commit();
            assertEquals(epoch,huge.uploadedEpoch);
        }
    }
    @Test void discardedSecondViewRestoresNewMeshForSameCamera() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency();
            long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var c=capture(); var a=mesh(112); var b=mesh(112);
            put(c,0,a,0,1,1);
            residency.prepare(c,frame,16,17,true); residency.commit();
            put(c,1,b,16,2,1); c.checkedResidencyEpoch=epoch; c.uploadCandidates.add(1);
            residency.prepare(c,frame,16,17,true);
            assertEquals(1,count(frame,1));
            var transition=residency.stageCommit();
            transition.rollback().run();
            assertEquals(0,b.layers[0].slotId);
            assertEquals(-1,b.instanceSlot);
            assertEquals(0,c.checkedResidencyEpoch);
            assertEquals(epoch,a.uploadedEpoch);
            residency.prepare(c,frame,16,17,true);
            assertEquals(1,count(frame,1));
            assertEquals(b.layers[0].slotId,id(frame,1,0,32));
        }
    }

    @Test void capturedUploadCandidatesAreConsumedOnceAndInvalidatedByEpochChanges() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var c=capture(); var resident=mesh(112);
            put(c,0,resident,0,1,1);
            residency.prepare(c,frame,16,17); residency.stageCommit();
            var added=mesh(112); put(c,1,added,16,2,1);
            c.checkedResidencyEpoch=epoch; c.uploadCandidates.add(1);
            residency.prepare(c,frame,16,17);
            assertEquals(1,count(frame,1)); assertEquals(2,count(frame,3));
            // Retry after a preparation that did not commit.
            residency.prepare(c,frame,16,17);
            assertEquals(1,count(frame,1)); residency.stageCommit();
            c.checkedResidencyEpoch=epoch; c.uploadCandidates.clear();
            residency.reset(NativeRenderer.nextFrameId());
            residency.prepare(c,frame,16,17);
            assertEquals(2,count(frame,1));
        }
    }
    @Test void closedFallbackSurvivesMultipleFramesThenEvictsAfterReplacement() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var old=mesh(112); var c=capture(); put(c,0,old,0,100,1);
            residency.prepare(c,frame,16,17); residency.commit(); residency.pendingRemovals.add(old);
            var replacement=mesh(2*1024*1024);
            for(int i=0;i<2;i++) {
                c.clear(); put(c,0,replacement,0,100,1); put(c,1,mesh(4*1024*1024),16+i*16,1,1);
                residency.prepare(c,frame,16,17);
                assertEquals(old.layers[0].slotId,id(frame,3,0,24)); assertEquals(0,count(frame,2));
                residency.commit(); assertEquals(epoch,old.uploadedEpoch); assertTrue(residency.pendingRemovals.contains(old));
            }
            residency.prepare(c,frame,16,17);
            assertEquals(replacement.layers[0].slotId,id(frame,3,0,24)); assertEquals(old.layers[0].slotId,id(frame,2,0,8));
            assertEquals(epoch,old.uploadedEpoch); assertEquals(0,replacement.uploadedEpoch);
            residency.commit(); assertEquals(0,old.uploadedEpoch); assertEquals(0,old.layers[0].slotId); assertEquals(epoch,replacement.uploadedEpoch);
            assertTrue(residency.pendingRemovals.isEmpty()); assertEquals(3,residency.residentCount());
            // The old removal must not erase the new occupant of the same section.
            residency.pendingRemovals.add(replacement); c.clear();
            var next=mesh(2*1024*1024); put(c,0,next,0,100,1); put(c,1,mesh(4*1024*1024),64,1,1);
            residency.prepare(c,frame,16,17); assertEquals(replacement.layers[0].slotId,id(frame,3,0,24));
            c.clear(); residency.prepare(c,frame,16,17);
            assertEquals(replacement.layers[0].slotId,id(frame,2,0,8)); residency.commit(); assertEquals(0,replacement.uploadedEpoch);
            assertEquals(0,replacement.layers[0].slotId);
        }
    }
    @Test void zeroMaskIsActiveWithoutInstancesAndLeavingViewReclaimsClosedMesh() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId());
            var p=mesh(112); var c=capture(); put(c,0,p,0,1,0);
            residency.prepare(c,frame,16,17); residency.commit(); residency.pendingRemovals.add(p);
            residency.prepare(c,frame,16,17); assertEquals(0,count(frame,3)); assertEquals(0,count(frame,4)); assertEquals(0,count(frame,2));
            residency.commit(); assertEquals(1,residency.pendingRemovals.size());
            c.clear(); residency.prepare(c,frame,16,17); assertEquals(p.layers[0].slotId,id(frame,2,0,8)); residency.commit();
            assertEquals(0,p.uploadedEpoch); assertEquals(0,p.layers[0].slotId); assertEquals(0,residency.residentCount()); assertTrue(residency.pendingRemovals.isEmpty());
        }
    }
    @Test void uncommittedUploadRetainsBytesAndNewEpochRequiresFreshPayload() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var p=mesh(112); var c=capture(); put(c,0,p,0,1,1);
            residency.prepare(c,frame,16,17); assertEquals(0,p.uploadedEpoch); assertEquals(0,residency.residentCount());
            assertEquals(1,p.layers[0].slotId); assertTrue(p.layers[0].hasVertices());
            residency.prepare(c,frame,16,17); assertEquals(1,count(frame,1)); assertEquals(1,p.layers[0].slotId); residency.commit();
            residency.pendingRemovals.add(p); c.clear(); residency.prepare(c,frame,16,17);
            assertEquals(epoch,p.uploadedEpoch); assertEquals(1,residency.pendingRemovals.size());
            assertFalse(p.layers[0].hasVertices());
            long nextEpoch=NativeRenderer.nextFrameId(); residency.reset(nextEpoch);
            assertEquals(0,p.layers[0].slotId);
            assertEquals(-1,p.instanceSlot);
            c.epoch=2; put(c,0,p,0,1,1); residency.prepare(c,frame,16,17);
            assertEquals(0,count(frame,1)); assertEquals(0,count(frame,3));
            var fresh=new SectionPayload(2,new MeshPayload(2,ByteBuffer.allocate(112)));
            put(c,0,fresh,0,1,1); residency.prepare(c,frame,16,17);
            assertEquals(1,count(frame,1)); assertEquals(1,fresh.layers[0].slotId); assertTrue(fresh.layers[0].hasVertices()); residency.commit();
            assertTrue(fresh.layers[0].hasVertices());
            frame.clearBorrowed();
            assertFalse(fresh.layers[0].hasVertices()); assertEquals(nextEpoch,fresh.uploadedEpoch);
        }
    }
    @Test void closeBetweenCaptureAndFirstUploadIsRetainedUntilTheFollowingFrame() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId());
            var p=mesh(112); var c=capture(); put(c,0,p,0,1,1);
            residency.pendingRemovals.add(p); residency.prepare(c,frame,16,17); residency.commit();
            assertEquals(1,residency.pendingRemovals.size()); assertEquals(1,residency.residentCount());
            c.clear(); residency.prepare(c,frame,16,17); assertEquals(p.layers[0].slotId,id(frame,2,0,8)); residency.commit();
            assertEquals(0,residency.residentCount()); assertTrue(residency.pendingRemovals.isEmpty());
        }
    }
    @Test void threeLayersAreAdoptedTogetherAndTransparentIndicesAreFrozen() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var solid=new MeshPayload(1,ByteBuffer.allocate(112),0);
            var cutout=new MeshPayload(1,ByteBuffer.allocate(112),1);
            var translucent=new MeshPayload(1,ByteBuffer.allocate(112),2);
            var first=indices(translucent,0,1,2,2,3,0); translucent.indices=first;
            var section=new SectionPayload(1,solid,cutout,translucent); var c=capture();
            put(c,0,section,0,1,0x010101);
            var later=indices(translucent,3,2,1,1,0,3); translucent.indices=later;
            residency.prepare(c,frame,16,17);
            assertEquals(3,count(frame,1)); assertEquals(3,count(frame,3)); assertEquals(0,section.uploadedEpoch);
            residency.commit(); assertSame(first,section.uploadedIndices); assertEquals(epoch,section.uploadedEpoch);
            // A new snapshot is not committed until native submission succeeds; vertex uploads stay zero.
            put(c,0,section,0,1,0x010101); residency.prepare(c,frame,16,17);
            assertEquals(0,count(frame,1)); assertEquals(1,count(frame,0)); assertSame(first,section.uploadedIndices);
            residency.commit(); assertSame(later,section.uploadedIndices);
            residency.prepare(c,frame,16,17); assertEquals(0,count(frame,0)); assertEquals(0,residency.uploadBytes);
            residency.pendingRemovals.add(section); c.clear(); residency.prepare(c,frame,16,17);
            assertEquals(3,count(frame,2)); residency.commit(); assertEquals(0,residency.residentCount());
        }
    }
    @Test void replacementWithoutWaterDoesNotRetainOldTransparentLayer() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId()); var c=capture();
            var water=new MeshPayload(1,ByteBuffer.allocate(112),2);
            water.indices=indices(water,0,1,2,2,3,0);
            var old=new SectionPayload(1,water); put(c,0,old,0,1,0x010000);
            residency.prepare(c,frame,16,17); residency.commit(); residency.pendingRemovals.add(old);
            var next=mesh(112); c.clear(); put(c,0,next,0,1,1); residency.prepare(c,frame,16,17);
            assertEquals(1,count(frame,3)); assertEquals(next.layers[0].slotId,id(frame,3,0,24));
            assertEquals(water.slotId,id(frame,2,0,8)); residency.commit();
            assertEquals(1,residency.residentCount());
        }
    }
    @Test void denseSlotIdsRecycleOnEvictionAndRollbackWhenUncommitted() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); residency.reset(NativeRenderer.nextFrameId()); var c=capture();
            var a=mesh(112); var b=mesh(112); var d=mesh(112);
            put(c,0,a,0,1,1); put(c,1,b,16,2,1);
            residency.prepare(c,frame,16,17);
            assertEquals(1,a.layers[0].slotId); assertEquals(2,b.layers[0].slotId);
            // Rollback without commit: leaving view must free slots 1 and 2 back to the pool.
            c.clear(); put(c,0,d,32,1,1);
            residency.prepare(c,frame,16,17);
            assertEquals(0,a.layers[0].slotId); assertEquals(0,b.layers[0].slotId);
            assertEquals(-1,a.instanceSlot); assertEquals(-1,b.instanceSlot);
            assertTrue(d.layers[0].slotId==1 || d.layers[0].slotId==2);
            residency.commit();
            int keptSlot=d.layers[0].slotId;
            int keptInstance=d.instanceSlot;
            // Evicting d recycles keptSlot for the next uploaded section.
            residency.pendingRemovals.add(d); c.clear();
            residency.prepare(c,frame,16,17);
            var eviction=residency.stageCommit();
            assertEquals(0,d.layers[0].slotId);
            assertEquals(-1,d.instanceSlot);
            var e=mesh(112); put(c,0,e,48,1,1);
            residency.prepare(c,frame,16,17);
            assertEquals(keptSlot,e.layers[0].slotId);
            assertEquals(keptInstance,e.instanceSlot);
            residency.stageCommit().rollback().run();
            eviction.rollback().run();
            assertEquals(-1,e.instanceSlot);
            assertEquals(keptInstance,d.instanceSlot);
            assertEquals(keptSlot,d.layers[0].slotId);
            c.clear();
            residency.prepare(c,frame,16,17);
            var retryEviction=residency.stageCommit();
            put(c,0,e,48,1,1);
            residency.prepare(c,frame,16,17);
            // The last view borrowed the freed slots but failed before recording its transition.
            retryEviction.rollback().run();
            assertEquals(-1,e.instanceSlot);
            assertEquals(keptInstance,d.instanceSlot);
            assertEquals(keptSlot,d.layers[0].slotId);
        }
    }
    @Test void releasedVerticesKeepTransparentIndexValidationAndOwnedSnapshots() {
        var source=ByteBuffer.allocate(128); source.position(8).limit(120);
        var mesh=new MeshPayload(1,source.slice(),2); assertEquals(4,mesh.vertexCount);
        mesh.releaseVertices();
        assertFalse(mesh.hasVertices());
        for(int stride:new int[]{2,4}) {
            var bytes=ByteBuffer.allocate(6*stride).order(ByteOrder.nativeOrder());
            int[] values={0,1,2,2,3,0};
            for(int value:values) if(stride==2) bytes.putShort((short)value); else bytes.putInt(value);
            bytes.flip(); var snapshot=mesh.copyIndices(bytes,stride); bytes.putInt(0,0);
            assertNotNull(snapshot.directBuffer());
            var copied=new int[snapshot.count];
            snapshot.directBuffer().asIntBuffer().get(copied);
            assertArrayEquals(values,copied);
            snapshot.releaseBuffer();
            assertNull(snapshot.directBuffer());
            if(stride==2) bytes.putShort(0,(short)4); else bytes.putInt(0,4);
            assertThrows(IllegalArgumentException.class,()->mesh.copyIndices(bytes,stride));
        }
        assertThrows(IllegalArgumentException.class,()->mesh.copyIndices(ByteBuffer.allocate(5),2));
    }
    @Test void backgroundSixAxisGroupingReordersQuadsComputesBoundsAndPacksWireframeGroups() {
        int[] order={6,0,5,2,0,1,4,3,6,2,5};
        ByteBuffer src=ByteBuffer.allocate(order.length*112).order(ByteOrder.nativeOrder());
        for(int i=0;i<order.length;i++) src.put(makeQuad(order[i],(float)i));
        src.flip();
        var mesh=new MeshPayload(TerrainCapture.epoch(),src,0);
        assertEquals(order.length*4,mesh.vertexCount);
        assertArrayEquals(new int[]{2,1,2,1,1,2,2},mesh.uploadLayout().groupQuadCounts);
        assertEquals(NativeFrameBuffer.LAYOUT_FLAG_FINITE_BOUNDS|NativeFrameBuffer.LAYOUT_FLAG_WIREFRAME_GROUPS,mesh.layoutFlags);
        assertNotNull(mesh.uploadLayout().originalQuadGroups);
        assertEquals((order.length*3+7)/8,mesh.uploadLayout().originalQuadGroups.length);
        // Verify unpacked 3-bit groups match input order exactly.
        for(int q=0;q<order.length;q++) {
            int bit=q*3,b=bit>>>3,shift=bit&7;
            int packed=Byte.toUnsignedInt(mesh.uploadLayout().originalQuadGroups[b]);
            if(shift>5) packed|=Byte.toUnsignedInt(mesh.uploadLayout().originalQuadGroups[b+1])<<8;
            assertEquals(order[q],(packed>>>shift)&7);
        }
        // Verify direct vertices are grouped in ascending group order 0..6 (color tag 0x12340000+group).
        var direct=mesh.directVertices();
        assertNotNull(direct);
        int cursor=0;
        for(int g=0;g<7;g++) {
            for(int k=0;k<mesh.uploadLayout().groupQuadCounts[g];k++) {
                assertEquals(0x12340000+g,direct.getInt(cursor*112+12));
                cursor++;
            }
        }
        mesh.releaseVertices();
        assertTrue(DirectUploadPool.pooledBytes()>=4096);
    }
    @Test void partialRefreshKeepsTheLastCommittedSortAfterItsDirectBufferReturnsToThePool() {
        TerrainCapture.activate(true);
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency();
            residency.reset(NativeRenderer.nextFrameId());
            var water=new MeshPayload(TerrainCapture.epoch(),ByteBuffer.allocate(112),2);
            var values=ByteBuffer.allocate(24).order(java.nio.ByteOrder.nativeOrder());
            for(int value:new int[]{0,1,2,2,3,0}) values.putInt(value);
            values.flip();
            var first=water.indices=water.copyIndices(values,4);
            var section=new SectionPayload(TerrainCapture.epoch(),water);
            var c=capture(); c.epoch=TerrainCapture.epoch();
            put(c,0,section,0,1,0x010000);
            residency.prepare(c,frame,16,17,true);
            residency.stageCommit().submitted().run();
            assertTrue(first.hasData());
            frame.clearBorrowed();
            assertFalse(first.hasData());
            var next=water.indices=water.copyIndices(values,4);
            TerrainCapture.recordDirtyIndices(section);
            c.fastReused=true; c.fadeNowMs=12400; c.fadeDurationMs=750;
            residency.prepare(c,frame,16,17,true);
            residency.stageCommit().submitted().run();
            assertSame(next,c.indices[0]);
            assertTrue(next.hasData());
            frame.clearBorrowed();
            assertFalse(next.hasData());
            c.fastReused=false;
            c.partialRefresh=true;
            c.indices[0]=first;
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,count(frame,0));
            assertSame(next,c.indices[0]);
        } finally { TerrainCapture.activate(false); }
    }

    @Test void firstTranslucentIndicesPublishedAfterCaptureMakeTheSectionResident() {
        TerrainCapture.activate(true);
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency();
            long epoch=NativeRenderer.nextFrameId();
            residency.reset(epoch);
            var water=new MeshPayload(TerrainCapture.epoch(),ByteBuffer.allocate(112),2);
            var section=new SectionPayload(TerrainCapture.epoch(),water);
            var c=capture(); c.epoch=TerrainCapture.epoch();
            put(c,0,section,0,1,0x010000);
            water.indices=indices(water,0,1,2,2,3,0);
            TerrainCapture.recordDirtyIndices(section);
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,count(frame,1));
            residency.commit();
            c.fastReused=true;
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,residency.reuseFlags());
            assertEquals(1,count(frame,1));
            residency.commit();
            assertEquals(epoch,section.uploadedEpoch);
        } finally { TerrainCapture.activate(false); }
    }

    @Test void reuseFastPathHandlesSteadyFadingAndDirtyTranslucentIndices() {
        TerrainCapture.activate(true);
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var solid=new MeshPayload(TerrainCapture.epoch(),ByteBuffer.allocate(112),0);
            var water=new MeshPayload(TerrainCapture.epoch(),ByteBuffer.allocate(112),2);
            var idx1=indices(water,0,1,2,2,3,0); water.indices=idx1;
            var section=new SectionPayload(TerrainCapture.epoch(),solid,water);
            var c=capture(); c.epoch=TerrainCapture.epoch(); put(c,0,section,0,1,0x010001); c.fadeStart[0]=12345;
            // Frame 1: full prepare + commit establishes cached visible & instances.
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,residency.reuseFlags());
            assertEquals(2,count(frame,3)); assertEquals(1,count(frame,4));
            long visible=descriptor(frame).get(JAVA_LONG,288);
            long origins=descriptor(frame).get(JAVA_LONG,296);
            long instances=descriptor(frame).get(JAVA_LONG,304);
            assertTrue(visible>0 && origins>0 && instances>0);
            residency.commit();

            // Frame 2: steady reuse sets both REUSE_VISIBLE and REUSE_INSTANCES with 0 visible/instance writes.
            c.fastReused=true;
            residency.prepare(c,frame,16,17,true);
            assertEquals(NativeRenderer.REUSE_VISIBLE|NativeRenderer.REUSE_INSTANCES,residency.reuseFlags());
            assertEquals(0,count(frame,0)); assertEquals(0,count(frame,1));
            assertEquals(0,count(frame,3)); assertEquals(0,count(frame,4));
            assertEquals(visible,descriptor(frame).get(JAVA_LONG,288));
            assertEquals(origins,descriptor(frame).get(JAVA_LONG,296));
            assertEquals(instances,descriptor(frame).get(JAVA_LONG,304));
            assertEquals(2,residency.effectiveVisibleCount());
            residency.commit();

            // Clock and duration are view constants, including disabling the fade mid-flight.
            c.fadeNowMs=12720; c.fadeDurationMs=0;
            residency.prepare(c,frame,16,17,true);
            assertEquals(NativeRenderer.REUSE_VISIBLE|NativeRenderer.REUSE_INSTANCES,residency.reuseFlags());
            assertEquals(0,count(frame,3)); assertEquals(0,count(frame,4));
            assertEquals(visible,descriptor(frame).get(JAVA_LONG,288));
            assertEquals(origins,descriptor(frame).get(JAVA_LONG,296));
            assertEquals(instances,descriptor(frame).get(JAVA_LONG,304));
            residency.commit();

            // A new upload timestamp on a dirty section changes only its instance slot.
            c.fastReused=false; c.fadeStart[0]=0x8000_0234;
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,residency.reuseFlags());
            assertEquals(2,count(frame,3)); assertEquals(1,count(frame,4));
            long fadedInstances=descriptor(frame).get(JAVA_LONG,304);
            assertEquals(visible,descriptor(frame).get(JAVA_LONG,288));
            assertEquals(origins,descriptor(frame).get(JAVA_LONG,296));
            assertTrue(fadedInstances>instances);
            assertEquals(0x8000_0234,frame.instanceSegment().get(JAVA_INT,12));
            residency.commit();

            // Frame 4: dirty translucent resort updates ONLY index buffer while keeping REUSE_VISIBLE | REUSE_INSTANCES.
            var idx2=indices(water,3,2,1,1,0,3); water.indices=idx2;
            TerrainCapture.recordDirtyIndices(section);
            c.fastReused=true;
            residency.prepare(c,frame,16,17,true);
            assertEquals(NativeRenderer.REUSE_VISIBLE|NativeRenderer.REUSE_INSTANCES,residency.reuseFlags());
            assertEquals(1,count(frame,0)); assertEquals(0,count(frame,1));
            assertEquals(0,count(frame,3)); assertEquals(0,count(frame,4));
            assertEquals(fadedInstances,descriptor(frame).get(JAVA_LONG,304));
            residency.commit();
            assertSame(idx2,section.uploadedIndices);

            // Frame 5: uncommitted fast prepare falls back to full upload while preserving unchanged versions.
            residency.prepare(c,frame,16,17,true);
            assertEquals(NativeRenderer.REUSE_VISIBLE|NativeRenderer.REUSE_INSTANCES,residency.reuseFlags());
            // Do NOT commit -> next prepare must fall back to full upload!
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,residency.reuseFlags());
            assertEquals(2,count(frame,3)); assertEquals(1,count(frame,4));
            assertEquals(visible,descriptor(frame).get(JAVA_LONG,288));
            assertEquals(origins,descriptor(frame).get(JAVA_LONG,296));
            assertEquals(fadedInstances,descriptor(frame).get(JAVA_LONG,304));
            residency.commit();
        }
    }

    @Test void reorderedSectionsReuseInstanceSlotsAndOnlyChangedSlotsCrossTheAbi() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency();
            residency.reset(NativeRenderer.nextFrameId());
            var a=new SectionPayload(1,new MeshPayload(1,ByteBuffer.allocate(112),0),new MeshPayload(1,ByteBuffer.allocate(112),1));
            var b=mesh(112); var c=capture();
            put(c,0,a,0,1,0x0101); put(c,1,b,16,2,1);
            residency.prepare(c,frame,16,17,true);
            assertEquals(2,descriptor(frame).get(JAVA_INT,320));
            assertEquals(0,a.instanceSlot); assertEquals(1,b.instanceSlot);
            assertEquals(a.instanceSlot,frame.visibleSegment().get(JAVA_INT,16));
            assertEquals(a.instanceSlot,frame.visibleSegment().get(JAVA_INT,24+16));
            assertEquals(b.instanceSlot,frame.visibleSegment().get(JAVA_INT,48+16));
            long initial=descriptor(frame).get(JAVA_LONG,304);
            residency.commit();
            put(c,0,b,16,1,1); put(c,1,a,0,2,0x0101);
            residency.prepare(c,frame,16,17,true);
            assertEquals(initial,descriptor(frame).get(JAVA_LONG,328));
            assertEquals(0,count(frame,4));
            assertEquals(b.instanceSlot,frame.visibleSegment().get(JAVA_INT,16));
            residency.commit();
            c.fastReused=false; c.fadeStart[0]=12345;
            residency.prepare(c,frame,16,17,true);
            assertEquals(1,count(frame,4));
            assertEquals(b.instanceSlot,descriptor(frame).get(ADDRESS,312).reinterpret(4).get(JAVA_INT,0));
            long updated=descriptor(frame).get(JAVA_LONG,304);
            residency.commit();
            c.finish(1);
            residency.prepare(c,frame,16,17,true);
            assertEquals(0,count(frame,4));
            assertEquals(updated,descriptor(frame).get(JAVA_LONG,304));
            assertEquals(updated,descriptor(frame).get(JAVA_LONG,328));
        }
    }

    @Test void fullRescanPreservesVersionsWhenUnchangedAndResetsOnNewEpoch() {
        try(var frame=new NativeFrameBuffer()) {
            var residency=new TerrainResidency(); long epoch=NativeRenderer.nextFrameId(); residency.reset(epoch);
            var a=mesh(112); var b=mesh(112); var c=capture();
            put(c,0,a,0,1,1); put(c,1,b,16,2,1);
            residency.prepare(c,frame,16,17,false);
            long v1=descriptor(frame).get(JAVA_LONG,288);
            long o1=descriptor(frame).get(JAVA_LONG,296);
            long i1=descriptor(frame).get(JAVA_LONG,304);
            assertTrue(v1>0 && o1>0 && i1>0);
            residency.commit();

            // Non-canReuse full rescan with identical visible set, origins and fade starts keeps all three versions.
            c.fastReused=false;
            residency.prepare(c,frame,16,17,false);
            assertEquals(0,residency.reuseFlags());
            assertEquals(v1,descriptor(frame).get(JAVA_LONG,288));
            assertEquals(o1,descriptor(frame).get(JAVA_LONG,296));
            assertEquals(i1,descriptor(frame).get(JAVA_LONG,304));
            residency.commit();

            // Changing only the fade start advances instancesVersion without disturbing visibleVersion or originsVersion.
            c.fadeStart[0]=12345;
            residency.prepare(c,frame,16,17,false);
            long i2=descriptor(frame).get(JAVA_LONG,304);
            assertEquals(v1,descriptor(frame).get(JAVA_LONG,288));
            assertEquals(o1,descriptor(frame).get(JAVA_LONG,296));
            assertTrue(i2>i1);
            residency.commit();

            // Changing origin advances originsVersion and instancesVersion while keeping visibleVersion.
            c.originX[1]=32;
            residency.prepare(c,frame,16,17,false);
            long o2=descriptor(frame).get(JAVA_LONG,296);
            long i3=descriptor(frame).get(JAVA_LONG,304);
            assertEquals(v1,descriptor(frame).get(JAVA_LONG,288));
            assertTrue(o2>o1);
            assertTrue(i3>i2);
            residency.commit();

            // Hiding a resident slot changes only the visible list.
            c.finish(1);
            residency.prepare(c,frame,16,17,false);
            long v2=descriptor(frame).get(JAVA_LONG,288);
            long o3=descriptor(frame).get(JAVA_LONG,296);
            long i4=descriptor(frame).get(JAVA_LONG,304);
            assertTrue(v2>v1);
            assertEquals(o2,o3);
            assertEquals(i3,i4);
            residency.commit();

            // Reset clears versions so next epoch assigns fresh versions even on empty capture.
            residency.reset(NativeRenderer.nextFrameId());
            c.clear();
            residency.prepare(c,frame,16,17,false);
            assertTrue(descriptor(frame).get(JAVA_LONG,288)>v2);
            assertTrue(descriptor(frame).get(JAVA_LONG,296)>o3);
            assertTrue(descriptor(frame).get(JAVA_LONG,304)>i4);
        }
    }
    @Test void directUploadPoolReusesPowerOfTwoBuffersAndRespectsCap() {
        DirectUploadPool.clearForTests();
        var a=DirectUploadPool.acquire(100,TerrainCapture.epoch());
        assertEquals(4096,a.capacity()); assertEquals(100,a.limit());
        DirectUploadPool.release(a,TerrainCapture.epoch());
        assertEquals(4096,DirectUploadPool.pooledBytes());
        var b=DirectUploadPool.acquire(3000,TerrainCapture.epoch());
        assertSame(a,b); assertEquals(3000,b.limit());
        assertEquals(0,DirectUploadPool.pooledBytes());
        DirectUploadPool.release(b,TerrainCapture.epoch());
    }
}
