package dev.raynium.client.takeover.scene;

import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class SceneGeometryTest {
    @Test void preservesVanillaQuadAndFanWinding() {
        assertArrayEquals(new int[]{0,1,2,2,3,0,4,5,6,6,7,4},SceneGeometry.sequentialIndices(PrimitiveTopology.QUADS,8));
        assertArrayEquals(new int[]{0,1,2,3,2,1,4,5,6,7,6,5},SceneGeometry.sequentialIndices(PrimitiveTopology.LINES,8));
        assertArrayEquals(new int[]{0,1,2,0,2,3,0,3,4},SceneGeometry.sequentialIndices(PrimitiveTopology.TRIANGLE_FAN,5));
        assertThrows(IllegalArgumentException.class,()->SceneGeometry.sequentialIndices(PrimitiveTopology.QUADS,5));
        assertThrows(IllegalArgumentException.class,()->SceneGeometry.sequentialIndices(PrimitiveTopology.TRIANGLE_FAN,2));
    }
}
