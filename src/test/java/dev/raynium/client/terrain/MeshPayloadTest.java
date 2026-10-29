package dev.raynium.client.terrain;
import java.nio.*;
import java.util.Arrays;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class MeshPayloadTest {
    private static ByteBuffer vertices(float[][] positions) {
        var b=ByteBuffer.allocate(positions.length*28).order(ByteOrder.nativeOrder());
        for(var p:positions) b.putFloat(p[0]).putFloat(p[1]).putFloat(p[2]).putInt(-1).putFloat(0).putFloat(0).putInt(0);
        return b.flip();
    }
    @Test void ownsDataAndUsesActualOutlyingBounds() {
        var source=vertices(new float[][]{{32,0,0},{32,1,0},{32,1,1},{32,0,1}});
        var payload=new MeshPayload(1,source);
        assertEquals(4,payload.groupVertices[0]);
        byte[] copy=payload.vertices.clone(); source.putFloat(0,99);
        assertArrayEquals(copy,payload.vertices);
        assertEquals(0,payload.visibleGroups(31,0,0)&1);
        assertEquals(1,payload.visibleGroups(33,0,0)&1);
        assertEquals(1,payload.visibleGroups(32,0,0)&1);
        assertEquals(127,payload.visibleGroups(Double.NaN,0,0));
    }
    @Test void nonplanarAndInconsistentQuadsRemainGeneral() {
        for(float[][] positions:new float[][][]{
            {{0,0,0},{0,1,0},{.1f,1,1},{0,0,1}},
            {{0,0,0},{0,1,0},{0,1,1},{0,2,0}},
            {{0,0,0},{0,0,0},{0,0,0},{0,0,0}}}) {
            var payload=new MeshPayload(1,vertices(positions));
            assertEquals(4,payload.groupVertices[6]);
            assertEquals(4,Arrays.stream(payload.groupVertices).sum());
        }
    }
    @Test void preservesWithinGroupOrderAndAllVertexBytes() {
        var a=vertices(new float[][]{{1,0,0},{1,1,0},{1,1,1},{1,0,1}});
        var b=vertices(new float[][]{{2,0,0},{2,1,0},{2,1,1},{2,0,1}});
        var source=ByteBuffer.allocate(224).put(a).put(b).flip();
        var payload=new MeshPayload(1,source);
        assertArrayEquals(source.array(),payload.vertices);
        assertEquals(8,payload.groupVertices[0]);
    }
}
