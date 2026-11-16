package dev.raynium.client.takeover.scene;

import com.mojang.blaze3d.vertex.CompactVectorArray;
import dev.raynium.bridge.NativeSceneFrame;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class SceneFeaturesTest {
    @Test void offscreenQuadsUseDepthAndPreserveEqualDepthOrder() {
        var centers=new CompactVectorArray(3);
        centers.set(0,6,8,-2);
        centers.set(1,10,8,2);
        centers.set(2,4,8,-2);
        assertArrayEquals(new int[]{0,2,1},SceneFeatures.quadSorting(NativeSceneFrame.OFFSCREEN,false).sort(centers));
        assertArrayEquals(new int[]{1,0,2},SceneFeatures.quadSorting(NativeSceneFrame.HAND,false).sort(centers));
        assertArrayEquals(new int[]{1,0,2},SceneFeatures.quadSorting(-1,false).sort(centers));
    }

    @Test void globalImprovedTransparencyPreservesSubmittedIndicesInEveryView() {
        assertNull(SceneFeatures.quadSorting(-1,true));
        assertNull(SceneFeatures.quadSorting(NativeSceneFrame.HAND,true));
        assertNull(SceneFeatures.quadSorting(NativeSceneFrame.OFFSCREEN,true));
    }
}
