package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.util.List;
import org.junit.jupiter.api.Test;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

class NativeFrameBufferTest {
    @Test void oversizedStorageShrinksOnlyAfterConsecutiveLowFrames() {
        try(var storage=new NativeFrameBuffer.Storage()) {
            storage.reserve(17L<<20);
            var old=storage.segment;
            for(int i=0;i<119;i++) storage.reserve(8);
            assertSame(old,storage.segment);
            storage.reserve(10L<<20);
            for(int i=0;i<119;i++) storage.reserve(8);
            assertSame(old,storage.segment);
            storage.reserve(8);
            assertFalse(old.scope().isAlive());
            assertEquals(8,storage.segment.byteSize());
        }
    }
    @Test void clearsEveryArrayPointerAndCountOnTheNextEmptyFrame() {
        try(var buffer=new NativeFrameBuffer()) {
            var frame=new TerrainFixture();
            frame.indices=List.of(new TerrainFixture.Index(1,new int[]{0,1,2,2,3,0}));
            frame.meshes=List.of(new TerrainFixture.Mesh(1,new byte[112],4,0));
            frame.visible=List.of(new TerrainFixture.Visible(1,1,7,0,1));
            frame.instances=List.of(new TerrainFixture.Instance(1,2,3,.5f));
            frame.removedMeshes=new long[]{2};
            var descriptor=frame.marshal(buffer);
            assertEquals(336,descriptor.byteSize()); assertEquals(336,descriptor.get(JAVA_INT,0));
            for(int i=0;i<5;i++) {
                assertNotEquals(MemorySegment.NULL,descriptor.get(ADDRESS,24+i*8L));
                assertEquals(1,descriptor.get(JAVA_INT,64+i*4L));
            }
            var next=new TerrainFixture(); next.epoch=2; next.outputSequence=8;
            assertSame(descriptor,next.marshal(buffer));
            for(int i=0;i<5;i++) {
                assertEquals(MemorySegment.NULL,descriptor.get(ADDRESS,24+i*8L));
                assertEquals(0,descriptor.get(JAVA_INT,64+i*4L));
            }
            assertEquals(2,descriptor.get(JAVA_LONG,8)); assertEquals(8,descriptor.get(JAVA_LONG,16));
        }
    }
    @Test void pooledUploadPreservesLargeMeshBytesAndLayout() {
        try(var buffer=new NativeFrameBuffer()) {
            var frame=new TerrainFixture(); byte[] vertices=new byte[11_200_000];
            vertices[0]=17; vertices[vertices.length-1]=29;
            frame.indices=List.of(new TerrainFixture.Index(1,new int[]{7,8,9}));
            frame.meshes=List.of(new TerrainFixture.Mesh(1,vertices,400_000,0));
            var descriptor=frame.marshal(buffer);
            var indices=descriptor.get(ADDRESS,24).reinterpret(24);
            var mesh=descriptor.get(ADDRESS,32).reinterpret(32);
            var copied=mesh.get(ADDRESS,8).reinterpret(vertices.length);
            assertEquals(0,copied.address()%8);
            assertEquals(17,copied.get(JAVA_BYTE,0)); assertEquals(29,copied.get(JAVA_BYTE,vertices.length-1));
            assertEquals(400_000,mesh.get(JAVA_INT,16));
            assertNotEquals(MemorySegment.NULL,mesh.get(ADDRESS,24));

            frame.marshal(buffer);
            assertEquals(29,mesh.get(ADDRESS,8).reinterpret(vertices.length).get(JAVA_BYTE,vertices.length-1));
        }
    }
    @Test void publicDirectMeshAndIndexOwnBytesAndKeepPreanalyzedLayout() {
        try(var buffer=new NativeFrameBuffer()) {
            int[] counts={1,0,0,0,0,0,1};
            float[] min={0f,1f,2f},max={3f,4f,5f};
            byte[] wf={0x30};
            buffer.begin(1,0,1,1,NativeFrameBuffer.alignedBytes(wf.length));
            try(var source=java.lang.foreign.Arena.ofConfined()) {
                var vertices=source.allocate(224,8);
                var indices=source.allocate(24,8);
                vertices.set(JAVA_INT,0,76); indices.set(JAVA_INT,0,39);
                buffer.meshDirect(7L,vertices,8,0,source,counts,
                    NativeFrameBuffer.LAYOUT_FLAG_FINITE_BOUNDS|NativeFrameBuffer.LAYOUT_FLAG_WIREFRAME_GROUPS,min,max,wf);
                buffer.indexDirect(9L,indices,6,source);
                vertices.fill((byte)0); indices.fill((byte)0);
            }
            var descriptor=buffer.finish(new TerrainFixture());
            var idxUpdate=descriptor.get(ADDRESS,24).reinterpret(24);
            assertEquals(9L,idxUpdate.get(JAVA_LONG,0));
            assertEquals(39,idxUpdate.get(ADDRESS,8).reinterpret(4).get(JAVA_INT,0));
            assertEquals(6,idxUpdate.get(JAVA_INT,16));
            var meshUpdate=descriptor.get(ADDRESS,32).reinterpret(32);
            assertEquals(7L,meshUpdate.get(JAVA_LONG,0));
            assertEquals(76,meshUpdate.get(ADDRESS,8).reinterpret(4).get(JAVA_INT,0));
            assertEquals(8,meshUpdate.get(JAVA_INT,16));
            assertEquals(0,meshUpdate.get(JAVA_INT,20));
            var layout=meshUpdate.get(ADDRESS,24).reinterpret(72);
            assertEquals(1,layout.get(JAVA_INT,0));
            assertEquals(1,layout.get(JAVA_INT,24));
            assertEquals(3,layout.get(JAVA_INT,28));
            assertEquals(2f,layout.get(JAVA_FLOAT,40));
            assertEquals(5f,layout.get(JAVA_FLOAT,52));
            assertEquals(1,layout.get(JAVA_INT,64));
            assertEquals(0x30,layout.get(ADDRESS,56).reinterpret(1).get(JAVA_BYTE,0));
        }
    }
    @Test void directWriterKeepsMasksAndMultipleInstanceRanges() {
        try(var buffer=new NativeFrameBuffer()) {
            buffer.begin(2,0,0,0,0);
            buffer.instance(3,4,5,.75f); buffer.instance(6,7,8,1);
            buffer.visible(9,0x80000001,123,0,2);
            var descriptor=buffer.finish(new TerrainFixture());
            var visible=descriptor.get(ADDRESS,48).reinterpret(24);
            assertEquals(0x80000001,visible.get(JAVA_INT,8));
            assertEquals(123,visible.get(JAVA_INT,12)); assertEquals(2,visible.get(JAVA_INT,20));
            assertEquals(1,descriptor.get(JAVA_INT,76)); assertEquals(2,descriptor.get(JAVA_INT,80));
        }
    }
}
