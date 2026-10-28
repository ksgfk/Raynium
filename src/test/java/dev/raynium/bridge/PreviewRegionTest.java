package dev.raynium.bridge;

import dev.raynium.client.preview.PreviewOverlay.Region;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class PreviewRegionTest {
    @Test void quarterResolutionAndMarginsStayInsideMainTarget() {
        assertEquals(new Region(1424, 794, 480, 270), Region.of(1920, 1080));
        assertEquals(new Region(0, 0, 1, 1), Region.of(1, 1));
        for (int w : new int[]{1, 2, 17, 33, 853}) for (int h : new int[]{1, 3, 19, 479}) {
            var r = Region.of(w, h);
            assertTrue(r.x() >= 0 && r.y() >= 0 && r.width() >= 1 && r.height() >= 1);
            assertTrue(r.x() + r.width() <= w && r.y() + r.height() <= h);
        }
    }
}
