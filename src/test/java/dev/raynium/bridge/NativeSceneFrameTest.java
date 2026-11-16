package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import org.junit.jupiter.api.Test;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

class NativeSceneFrameTest {
    @Test void retainsSamplingViewAndCompleteSamplerIdentityAcrossTableGrowth() {
        try(var buffer=new NativeFrameBuffer()) {
            buffer.begin(0,0,0,0,0); var scene=buffer.scene();
            scene.texture(0,16,28,2,3,1.25f,8);
            for(int i=0;i<400;i++) scene.constant(0,0,256,256);
            var descriptor=buffer.finish(new NativeRenderer.Frame()); var bindings=descriptor.get(ADDRESS,336).reinterpret(401*48);
            assertEquals(1,bindings.get(JAVA_INT,0)); assertEquals(2,bindings.get(JAVA_INT,32)); assertEquals(3,bindings.get(JAVA_INT,36));
            assertEquals(1.25f,bindings.get(JAVA_FLOAT,40)); assertEquals(8,bindings.get(JAVA_INT,44));
            assertEquals(0,bindings.get(JAVA_LONG,48+32)); assertEquals(256,bindings.get(JAVA_LONG,48+16));
            assertThrows(IllegalArgumentException.class,()->scene.texture(0,16,0,0,1,Float.NaN,1));
        }
    }
    @Test void retainsPostRequestOrderAndResetsBlurBoundary() {
        try(var buffer=new NativeFrameBuffer()) {
            buffer.begin(0,0,0,0,0); var scene=buffer.scene(); int globals=scene.copy(MemorySegment.ofArray(new byte[256]));
            scene.effect(4,0,globals); scene.effect(2,0,globals); scene.effect(1,1,globals); scene.guiBlurSplit(7);
            var descriptor=buffer.finish(new NativeRenderer.Frame()); var effects=descriptor.get(ADDRESS,384).reinterpret(48);
            assertEquals(3,descriptor.get(JAVA_INT,392)); assertEquals(7,descriptor.get(JAVA_INT,396));
            assertEquals(4,effects.get(JAVA_INT,0)); assertEquals(2,effects.get(JAVA_INT,16)); assertEquals(1,effects.get(JAVA_INT,36));
            assertThrows(IllegalArgumentException.class,()->scene.effect(3,0,globals));
            assertThrows(IllegalArgumentException.class,()->scene.effect(2,1,globals));
            buffer.begin(0,0,0,0,0); descriptor=buffer.finish(new NativeRenderer.Frame());
            assertEquals(0,descriptor.get(JAVA_INT,392)); assertEquals(0,descriptor.get(JAVA_INT,396));
        }
    }
    @Test void retainsOffscreenRangesAndRejectsOpenOrNestedTargets() {
        try(var buffer=new NativeFrameBuffer()) {
            buffer.begin(0,0,0,0,0); var scene=buffer.scene(); scene.beginTarget(16,32,64,16,16);
            assertThrows(IllegalArgumentException.class,()->scene.beginTarget(16,0,0,16,16));
            assertThrows(IllegalStateException.class,()->buffer.finish(new NativeRenderer.Frame()));
            scene.draw(23,NativeSceneFrame.OFFSCREEN,-1,-1,0,0,0,0,0,-1,0,0,0,0,0,16,16); scene.endTarget();
            var descriptor=buffer.finish(new NativeRenderer.Frame()); var target=descriptor.get(ADDRESS,368).reinterpret(32);
            assertEquals(1,descriptor.get(JAVA_INT,376)); assertEquals(32,target.get(JAVA_INT,4)); assertEquals(64,target.get(JAVA_INT,8));
            assertEquals(1,target.get(JAVA_INT,24));
            buffer.begin(0,0,0,0,0); assertEquals(0,buffer.finish(new NativeRenderer.Frame()).get(JAVA_INT,376));
        }
    }
    @Test void resolvesCopiedBlobsAfterGrowthAndKeepsBorrowedGeometryUntilConsumption() {
        try(var buffer=new NativeFrameBuffer(); var arena=Arena.ofConfined()) {
            buffer.begin(0,0,0,0,0); var scene=buffer.scene();
            byte[] source={11,22,33,44}; int copy=scene.copy(MemorySegment.ofArray(source)); source[0]=99;
            var geometry=arena.allocate(64,8); geometry.set(JAVA_INT,32,0x12345678); int borrowed=scene.borrow(geometry);
            scene.copy(arena.allocate(2<<20,8));
            int first=scene.bindingCount(); scene.constant(0,copy,0,4);
            scene.draw(23,NativeSceneFrame.SOLID,borrowed,-1,1,0,0,first,1,-1,32,0,0,0,0,127,65);
            var frame=new NativeRenderer.Frame(); var descriptor=buffer.finish(frame);
            var blobs=descriptor.get(ADDRESS,328).reinterpret(48);
            assertEquals(11,blobs.get(ADDRESS,0).reinterpret(4).get(JAVA_BYTE,0));
            assertEquals(geometry.address(),blobs.get(ADDRESS,16).address());
            var draws=descriptor.get(ADDRESS,344).reinterpret(88); assertEquals(32,draws.get(JAVA_LONG,48));
            assertEquals(3,descriptor.get(JAVA_INT,352)); assertEquals(1,descriptor.get(JAVA_INT,360));
            assertEquals(blobs.get(ADDRESS,0).address(),buffer.finish(frame).get(ADDRESS,328).reinterpret(48).get(ADDRESS,0).address());
            buffer.begin(0,0,0,0,0); descriptor=buffer.finish(frame);
            assertEquals(MemorySegment.NULL,descriptor.get(ADDRESS,328)); assertEquals(0,descriptor.get(JAVA_INT,360));
        }
    }
}
