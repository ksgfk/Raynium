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

    @Test void closeReleasesAllFiveOffHeapStreams() {
        var storage=new CommandTape.Storage();
        var commands=storage.commands.append(16);
        var uploads=storage.uploads.append(16);
        var instances=storage.instanceUploads.append(16);
        var captures=storage.captureIds.append(8);
        var queries=storage.queryWrites.append(16);
        storage.close();
        assertFalse(commands.scope().isAlive());
        assertFalse(uploads.scope().isAlive());
        assertFalse(instances.scope().isAlive());
        assertFalse(captures.scope().isAlive());
        assertFalse(queries.scope().isAlive());
    }
}
