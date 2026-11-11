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
        assertEquals(0,payload.visibleElementMask(31,0,0)&1);
        assertEquals(1,payload.visibleElementMask(33,0,0)&1);
        assertEquals(1,payload.visibleElementMask(32,0,0)&1);
        assertEquals(1,payload.visibleElementMask(Double.NaN,0,0));
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
    private static byte[] groupQuad(int group) {
        float[][] p=new float[4][3];
        if(group<6) {
            int a=group/2,b=(a+1)%3,c=(a+2)%3;
            p[1][b]=1; p[2][b]=1; p[2][c]=1; p[3][c]=1;
            if((group&1)!=0) { var swap=p[1]; p[1]=p[3]; p[3]=swap; }
        }
        var data=vertices(p);
        for(int i=0;i<4;i++) data.putInt(i*28+12,0x12340000+group);
        return data.array();
    }
    @Test void compactMasksAndDirectSlicedBufferKeepAllSevenGroups() {
        var data=ByteBuffer.allocateDirect(7*112+26).order(ByteOrder.nativeOrder());
        data.position(13);
        for(int g=6;g>=0;g--) data.put(groupQuad(g));
        data.limit(data.position()).position(13);
        var payload=new MeshPayload(1,data.asReadOnlyBuffer());
        assertEquals(13,data.position());
        assertArrayEquals(new int[]{4,4,4,4,4,4,4},payload.groupVertices);
        for(int g=0;g<7;g++) assertArrayEquals(groupQuad(g),Arrays.copyOfRange(payload.vertices,g*112,(g+1)*112));
        assertEquals(127,payload.allElementMask);
        assertEquals(127,payload.visibleElementMask(.5,.5,.5));
        assertEquals(0b1101010,payload.visibleElementMask(-2,-2,-2));
        assertEquals(0b1010101,payload.visibleElementMask(2,2,2));
        assertEquals(127,payload.visibleElementMask(0,Double.NaN,0));
    }
    @Test void sparseGroupsUseCompactBitsAndCanCullEveryElement() {
        var data=ByteBuffer.allocate(224).put(groupQuad(5)).put(groupQuad(2)).flip();
        var payload=new MeshPayload(1,data);
        assertEquals(3,payload.allElementMask);
        assertEquals(0,payload.visibleElementMask(0,-2,2));
        assertEquals(1,payload.visibleElementMask(0,2,2));
        assertEquals(2,payload.visibleElementMask(0,-2,-2));
        assertEquals(3,payload.visibleElementMask(Double.NaN,0,0));
    }
    @Test void nonfiniteBoundsRemainConservativeAndInvalidStreamsFail() {
        var data=ByteBuffer.wrap(groupQuad(0)).order(ByteOrder.nativeOrder()); data.putFloat(0,Float.NaN);
        var payload=new MeshPayload(1,data);
        assertEquals(1,payload.allElementMask);
        assertEquals(1,payload.visibleElementMask(-100,0,0));
        assertEquals(0,new MeshPayload(1,ByteBuffer.allocate(0)).allElementMask);
        assertThrows(IllegalArgumentException.class,()->new MeshPayload(1,ByteBuffer.allocate(111)));
    }
    @Test void transparencyKeepsCompilerOrderAndOwnsBothIndexWidths() {
        var source=ByteBuffer.allocate(224).put(groupQuad(5)).put(groupQuad(0)).flip();
        var mesh=new MeshPayload(1,source,2);
        assertArrayEquals(source.array(),mesh.vertices); assertEquals(8,mesh.groupVertices[6]);
        assertEquals(1,mesh.allElementMask); assertEquals(1,mesh.visibleElementMask(-100,-100,-100));
        for(int stride:new int[]{2,4}) {
            var buffer=ByteBuffer.allocate(12*stride).order(ByteOrder.nativeOrder());
            int[] values={4,5,6,6,7,4,0,1,2,2,3,0};
            for(int value:values) if(stride==2) buffer.putShort((short)value); else buffer.putInt(value);
            buffer.flip(); var snapshot=mesh.copyIndices(buffer,stride);
            buffer.putInt(0,0); assertArrayEquals(values,snapshot.values());
        }
        assertThrows(IllegalArgumentException.class,()->mesh.copyIndices(ByteBuffer.allocate(5),2));
    }

}
