package dev.raynium.client.terrain;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class TerrainCaptureTest {
    @AfterEach void cleanup() { TerrainCapture.activate(false); }
    private static MeshPayload mesh() { return new MeshPayload(TerrainCapture.epoch(),ByteBuffer.allocate(112)); }
    @Test void shrinkingAndGrowingCaptureReleasesDiscardedPayloads() {
        var buffer=new TerrainCapture.Buffer(); var a=mesh(); var b=mesh();
        buffer.put(0,a,0,0,0,1,1,0); buffer.put(1024,b,0,0,0,1,1,0); buffer.finish(1025);
        assertTrue(buffer.payloads.length>=1025);
        buffer.finish(1);
        assertSame(a,buffer.payloads[0]);
        for(int i=1;i<buffer.payloads.length;i++) assertNull(buffer.payloads[i]);
        buffer.clear(); assertNull(buffer.payloads[0]); assertEquals(0,buffer.count);
    }
    @Test void frameResetInvalidationAndDeactivationReleaseSnapshotAndReferences() throws Exception {
        var field=TerrainCapture.class.getDeclaredField("BUFFER"); field.setAccessible(true);
        var snapshotFlag=TerrainCapture.class.getDeclaredField("hasSnapshot"); snapshotFlag.setAccessible(true);
        var buffer=(TerrainCapture.Buffer)field.get(null);
        for(int operation=0;operation<3;operation++) {
            TerrainCapture.activate(true); var payload=mesh();
            buffer.put(0,payload,0,0,0,1,1,0); buffer.finish(1); snapshotFlag.setBoolean(null,true);
            TerrainCapture.removed(payload);
            if(operation==0) TerrainCapture.beginFrame();
            else if(operation==1) TerrainCapture.invalidate();
            else TerrainCapture.activate(false);
            assertNull(TerrainCapture.snapshot()); assertEquals(0,buffer.count); assertNull(buffer.payloads[0]);
            var removed=new ArrayList<MeshPayload>(); TerrainCapture.drainRemoved(removed);
            if(operation==0) assertEquals(java.util.List.of(payload),removed);
            else assertTrue(removed.isEmpty());
        }
    }
    @Test void distanceKeysAreNonnegativeAndMonotone() {
        int last=-1;
        for(double distance:new double[]{0,.001,1,16,1e6,1e16,Double.POSITIVE_INFINITY,Double.NaN}) {
            int key=TerrainCapture.distanceKey(distance); assertTrue(key>=0); assertTrue(key>=last); last=key;
        }
    }
}
