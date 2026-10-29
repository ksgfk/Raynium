package dev.raynium.client.terrain;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;
class TextureTransferTest {
    @Test void failedFallbackIsReportedAndNeverBecomesSuccess() {
        var direct=new IllegalStateException("direct import failed");
        var split=new IllegalStateException("single mip unsupported");
        var failure=assertThrows(IllegalStateException.class,()->TextureTransfer.create("auto",()->{throw direct;},()->{throw split;},e->{}));
        assertSame(split,failure); assertSame(direct,failure.getSuppressed()[0]);
        assertSame(direct,assertThrows(IllegalStateException.class,()->TextureTransfer.create("direct",()->{throw direct;},()->"unexpected",e->{})));
        assertEquals("fallback",TextureTransfer.create("split",()->{throw direct;},()->"fallback",e->{}));
    }
}
