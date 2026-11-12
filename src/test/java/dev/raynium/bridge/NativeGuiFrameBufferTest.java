package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import org.junit.jupiter.api.Test;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

class NativeGuiFrameBufferTest {
    @Test void directGuiWriterMarshalsAndClearsBorrowedArrays() {
        try(var buffer=new NativeFrameBuffer()) {
            buffer.begin(0,0,0,0,0); buffer.beginGui(4,1);
            for(int i=0;i<4;i++) buffer.guiVertex(i,2*i,0,.25f,.75f,0x80402010);
            buffer.guiDraw(11,16,12,0,4,1,2,3,4);
            var frame=new NativeRenderer.Frame(); frame.guiWidth=320; frame.guiHeight=180; frame.clearColor=new float[]{.1f,.2f,.3f,.4f};
            var descriptor=buffer.finish(frame);
            assertEquals(4,descriptor.get(JAVA_INT,296)); assertEquals(1,descriptor.get(JAVA_INT,300));
            assertEquals(320,descriptor.get(JAVA_FLOAT,304)); assertEquals(.4f,descriptor.get(JAVA_FLOAT,324));
            var vertex=descriptor.get(ADDRESS,280).reinterpret(96);
            assertEquals(.75f,vertex.get(JAVA_FLOAT,16)); assertEquals(0x80402010,vertex.get(JAVA_INT,20));
            var draw=descriptor.get(ADDRESS,288).reinterpret(40);
            assertEquals(11,draw.get(JAVA_INT,0)); assertEquals(16,draw.get(JAVA_INT,4));
            assertEquals(4,draw.get(JAVA_INT,16)); assertEquals(0,draw.get(JAVA_INT,36));
            buffer.begin(0,0,0,0,0);
            assertSame(descriptor,buffer.finish(new NativeRenderer.Frame()));
            assertEquals(MemorySegment.NULL,descriptor.get(ADDRESS,280)); assertEquals(MemorySegment.NULL,descriptor.get(ADDRESS,288));
            assertEquals(0,descriptor.get(JAVA_INT,296)); assertEquals(0,descriptor.get(JAVA_INT,300));
        }
    }
}
