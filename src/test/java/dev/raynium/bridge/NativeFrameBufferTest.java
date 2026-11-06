package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.util.List;
import org.junit.jupiter.api.Test;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

class NativeFrameBufferTest {
    @Test void clearsEveryArrayPointerAndCountOnTheNextEmptyFrame() {
        try(var buffer=new NativeFrameBuffer()) {
            var frame=new NativeRenderer.Frame();
            frame.indices=List.of(new NativeRenderer.Index(1,new int[]{0,1,2,2,3,0}));
            frame.meshes=List.of(new NativeRenderer.Mesh(1,new byte[112],1,4,List.of(new NativeRenderer.Submesh(0,6,0))));
            frame.materials=List.of(new NativeRenderer.Material(1,0,1,1));
            frame.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
            frame.visible=List.of(new NativeRenderer.Visible(1,1,7,0,1));
            frame.instances=List.of(new NativeRenderer.Instance(1,2,3,.5f));
            frame.removedMeshes=new long[]{2}; frame.removedPrimitives=new long[]{2};
            frame.removedMaterials=new long[]{3}; frame.removedIndices=new long[]{4};
            var descriptor=buffer.marshal(frame);
            assertEquals(344,descriptor.byteSize()); assertEquals(344,descriptor.get(JAVA_INT,0));
            for(int i=0;i<10;i++) {
                assertNotEquals(MemorySegment.NULL,descriptor.get(ADDRESS,32+i*8L));
                assertEquals(1,descriptor.get(JAVA_INT,112+i*4L));
            }
            var next=new NativeRenderer.Frame(); next.epoch=2; next.outputSequence=8;
            assertSame(descriptor,buffer.marshal(next));
            for(int i=0;i<10;i++) {
                assertEquals(MemorySegment.NULL,descriptor.get(ADDRESS,32+i*8L));
                assertEquals(0,descriptor.get(JAVA_INT,112+i*4L));
            }
            assertEquals(2,descriptor.get(JAVA_LONG,8)); assertEquals(8,descriptor.get(JAVA_LONG,16));
            assertEquals(0,descriptor.get(JAVA_INT,156));
        }
    }
    @Test void uploadStorageGrowsPastBudgetWithIndexAlignmentAndIsReused() {
        try(var buffer=new NativeFrameBuffer()) {
            var frame=new NativeRenderer.Frame(); byte[] vertices=new byte[11_200_000];
            vertices[0]=17; vertices[vertices.length-1]=29;
            frame.indices=List.of(new NativeRenderer.Index(1,new int[]{7,8,9}));
            frame.meshes=List.of(new NativeRenderer.Mesh(1,vertices,1,400_000,List.of(new NativeRenderer.Submesh(0,3,0))));
            var descriptor=buffer.marshal(frame);
            var indices=descriptor.get(ADDRESS,32).reinterpret(24);
            var mesh=descriptor.get(ADDRESS,40).reinterpret(40);
            var copied=mesh.get(ADDRESS,8).reinterpret(vertices.length);
            assertEquals(16,copied.address()-indices.get(ADDRESS,8).address());
            assertEquals(17,copied.get(JAVA_BYTE,0)); assertEquals(29,copied.get(JAVA_BYTE,vertices.length-1));
            assertEquals(400_000,mesh.get(JAVA_INT,32));
            long address=copied.address();
            buffer.marshal(frame);
            assertEquals(address,mesh.get(ADDRESS,8).address());
        }
    }
    @Test void directWriterKeepsMasksAndMultipleInstanceRanges() {
        try(var buffer=new NativeFrameBuffer()) {
            buffer.begin(2,0,0,0,0,0,0);
            buffer.instance(3,4,5,.75f); buffer.instance(6,7,8,1);
            buffer.visible(9,0x80000001,123,0,2);
            var descriptor=buffer.finish(new NativeRenderer.Frame());
            var visible=descriptor.get(ADDRESS,96).reinterpret(24);
            assertEquals(0x80000001,visible.get(JAVA_INT,8));
            assertEquals(123,visible.get(JAVA_INT,12)); assertEquals(2,visible.get(JAVA_INT,20));
            assertEquals(1,descriptor.get(JAVA_INT,144)); assertEquals(2,descriptor.get(JAVA_INT,148));
        }
    }
}
