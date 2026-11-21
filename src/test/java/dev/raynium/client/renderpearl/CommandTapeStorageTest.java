package dev.raynium.client.renderpearl;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class CommandTapeStorageTest {
    @Test void independentStreamsDropOversizedArenasAfter120LowBatches() {
        try(var storage=new CommandTape.Storage()) {
            var transfer=storage.uploads.append(17L<<20);
            var instances=storage.instanceUploads.append(56);
            storage.reset();
            for(int i=0;i<119;i++) {
                storage.uploads.append(32);
                storage.reset();
            }
            assertTrue(transfer.scope().isAlive());
            storage.reset();
            assertFalse(transfer.scope().isAlive());
            assertTrue(instances.scope().isAlive());
        }
    }
}
