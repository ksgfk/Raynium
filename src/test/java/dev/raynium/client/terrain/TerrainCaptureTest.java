package dev.raynium.client.terrain;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class TerrainCaptureTest {
    @AfterEach void cleanup() { TerrainCapture.activate(false); }
    private static SectionPayload mesh() { return new SectionPayload(TerrainCapture.epoch(),new MeshPayload(TerrainCapture.epoch(),ByteBuffer.allocate(112))); }
    @Test void retiringWorldIgnoresLateWorkersAndReleasesLayoutAfterLastPin() {
        TerrainCapture.activate(true);
        var old=mesh();
        var layer=old.layers[0];
        var first=layer.pin();
        var second=layer.pin();
        TerrainCapture.invalidate();
        var fresh=mesh();
        fresh.layers[0].releaseVertices();
        long idle=DirectUploadPool.pooledBytes();
        assertTrue(idle>0);
        layer.releaseVertices();
        layer.unpin();
        assertNotNull(layer.uploadLayout());
        assertEquals(first.get(java.lang.foreign.ValueLayout.JAVA_BYTE,0),second.get(java.lang.foreign.ValueLayout.JAVA_BYTE,0));
        layer.unpin();
        assertNull(layer.uploadLayout());
        assertEquals(4,layer.vertexCount);
        var late=new MeshPayload(old.epoch,ByteBuffer.allocate(112));
        late.releaseVertices();
        TerrainCapture.removed(old);
        TerrainCapture.recordDirtyIndices(old);
        var events=new ArrayList<SectionPayload>();
        TerrainCapture.drainRemoved(events);
        TerrainCapture.drainDirtyIndices(events);
        assertTrue(events.isEmpty());
        assertEquals(idle,DirectUploadPool.pooledBytes());
    }
    @Test void largeSectionReusesEightMiBBucket() {
        DirectUploadPool.clearForTests();
        var buffer=DirectUploadPool.acquire((4<<20)+1,TerrainCapture.epoch());
        assertEquals(8<<20,buffer.capacity());
        DirectUploadPool.release(buffer,TerrainCapture.epoch());
        assertSame(buffer,DirectUploadPool.acquire(8<<20,TerrainCapture.epoch()));
    }
    @Test void refreshesMoreThan64DirtySlotsWithoutFullCapture() throws Exception {
        TerrainCapture.activate(true);
        var field=TerrainCapture.class.getDeclaredField("buffer");
        field.setAccessible(true);
        var buffer=(TerrainCapture.Buffer)field.get(null);
        for(int i=0;i<128;i++) {
            var payload=mesh();
            buffer.put(i,payload,i,0,0,0,1,0);
            TerrainCapture.recordDirtyIndices(payload);
        }
        buffer.finish(128);
        assertTrue(TerrainCapture.refreshDirty(TerrainCapture.epoch()));
        assertEquals(128,buffer.uploadCandidates.size());
    }
    @Test void largeCandidateSetDeduplicatesAfterGrowthAndGenerationWrap() {
        var buffer=new TerrainCapture.Buffer();
        buffer.ensureCapacity(2048);
        for(int pass=0;pass<2;pass++) for(int slot=1024;slot<1280;slot++) buffer.addCandidate(slot);
        assertEquals(256,buffer.uploadCandidates.size());
        buffer.generation=-1;
        buffer.resetCandidates();
        buffer.addCandidate(1024);
        assertArrayEquals(new int[]{1024},buffer.uploadCandidates.toIntArray());
    }
    @Test void shrinkingAndGrowingCaptureReleasesDiscardedPayloads() {
        var buffer=new TerrainCapture.Buffer(); var a=mesh(); var b=mesh();
        buffer.put(0,a,0,0,0,0,1,0); buffer.put(1024,b,0,0,0,0,1,0); buffer.finish(1025);
        assertTrue(buffer.payloads.length>=1025);
        buffer.finish(1);
        assertSame(a,buffer.payloads[0]);
        for(int i=1;i<buffer.payloads.length;i++) assertNull(buffer.payloads[i]);
        buffer.clear(); assertNull(buffer.payloads[0]); assertEquals(0,buffer.count);
    }
    @Test void sortedIndexUpdatesOnlyMarkTheCurrentOccupantOfACapturedSlot() {
        var buffer=new TerrainCapture.Buffer();
        var old=mesh(); var current=mesh();
        buffer.put(0,old,0,0,0,0,1,0);
        buffer.finish(1);
        buffer.put(0,current,0,0,0,0,1,0);
        buffer.refreshIndexCandidate(old);
        assertTrue(buffer.uploadCandidates.isEmpty());
        buffer.refreshIndexCandidate(current);
        buffer.refreshIndexCandidate(current);
        assertArrayEquals(new int[]{0},buffer.uploadCandidates.toIntArray());
        buffer.finish(0);
        assertFalse(buffer.contains(current));
    }
    @Test void frameResetInvalidationAndDeactivationReleaseSnapshotAndReferences() throws Exception {
        var field=TerrainCapture.class.getDeclaredField("buffer"); field.setAccessible(true);
        var snapshotFlag=TerrainCapture.class.getDeclaredField("hasSnapshot"); snapshotFlag.setAccessible(true);
        var capturedFlag=TerrainCapture.class.getDeclaredField("capturedSinceBeginFrame"); capturedFlag.setAccessible(true);
        var buffer=(TerrainCapture.Buffer)field.get(null);
        for(int operation=0;operation<3;operation++) {
            buffer=(TerrainCapture.Buffer)field.get(null);
            TerrainCapture.activate(true); var payload=mesh();
            buffer.put(0,payload,0,0,0,0,1,0); buffer.finish(1); snapshotFlag.setBoolean(null,true);
            capturedFlag.setBoolean(null,false);
            TerrainCapture.removed(payload);
            if(operation==0) TerrainCapture.beginFrame();
            else if(operation==1) TerrainCapture.invalidate();
            else TerrainCapture.activate(false);
            assertNull(TerrainCapture.snapshot()); assertEquals(0,buffer.count); assertNull(buffer.payloads[0]);
            var removed=new ArrayList<SectionPayload>(operation==0?1:0); TerrainCapture.drainRemoved(removed);
            if(operation==0) assertEquals(java.util.List.of(payload),removed);
            else assertTrue(removed.isEmpty());
        }
        // Consecutive active frame keeps warm snapshot buffer across one beginFrame(), then clears if no capture runs.
        TerrainCapture.activate(true);
        buffer=(TerrainCapture.Buffer)field.get(null);
        var activePayload=mesh();
        buffer.put(0,activePayload,0,0,0,0,1,0); buffer.finish(1);
        snapshotFlag.setBoolean(null,true); capturedFlag.setBoolean(null,true);
        TerrainCapture.beginFrame();
        assertNull(TerrainCapture.snapshot());
        assertEquals(1,buffer.count); assertSame(activePayload,buffer.payloads[0]);
        // Second beginFrame without a capture in between releases warm references.
        TerrainCapture.beginFrame();
        assertEquals(0,buffer.count); assertNull(buffer.payloads[0]);
    }
    @Test void dirtyIndicesQueueDrainsAndClearsOnInvalidate() {
        TerrainCapture.activate(true);
        var payload=mesh();
        TerrainCapture.recordDirtyIndices(payload);
        var drained=new ArrayList<SectionPayload>(1);
        TerrainCapture.drainDirtyIndices(drained);
        assertEquals(java.util.List.of(payload),drained);
        TerrainCapture.recordDirtyIndices(payload);
        TerrainCapture.invalidate();
        drained.clear();
        TerrainCapture.drainDirtyIndices(drained);
        assertTrue(drained.isEmpty());
    }
    @Test void distanceKeysAreNonnegativeAndMonotone() {
        int last=-1;
        for(double distance:new double[]{0,.001,1,16,1e6,1e16,Double.POSITIVE_INFINITY,Double.NaN}) {
            int key=TerrainCapture.distanceKey(distance); assertTrue(key>=0); assertTrue(key>=last); last=key;
        }
    }
    @Test void noopTerrainStagingBufferSkipsCopiesAndNeverFills() {
        try(var staging=new NoopTerrainStagingBuffer()) {
            var data=ByteBuffer.allocate(256);
            var first=staging.tryAppend(data);
            var second=staging.tryAppend(data);
            assertNotNull(first);
            assertSame(first,second);
            assertEquals(0,data.position());
            first.close();
            try(var uploader=staging.startUploading(null)) {
                uploader.checkValidFor(staging);
            }
        }
    }
    @Test void relativeMillisecondsPreserveUnsignedPrecisionAndReserveZero() {
        assertEquals(0,TerrainCapture.encodeFadeStartMs(0));
        assertEquals(1,TerrainCapture.encodeRelativeMillis(-1));
        assertEquals(1,TerrainCapture.encodeRelativeMillis(0));
        assertEquals(16_777_218,TerrainCapture.encodeRelativeMillis(16_777_217));
        assertEquals(0x8000_0000,TerrainCapture.encodeRelativeMillis(0x7fff_ffffL));
        assertEquals(0xffff_fffe,TerrainCapture.encodeRelativeMillis(0xffff_fffdL));
        assertEquals(0,TerrainCapture.encodeRelativeMillis(0xffff_fffeL));
        assertEquals(0,TerrainCapture.encodeRelativeMillis(0x1_0000_0000L));
    }
}
