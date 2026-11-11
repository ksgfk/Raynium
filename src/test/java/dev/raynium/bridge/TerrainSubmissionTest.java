package dev.raynium.bridge;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class TerrainSubmissionTest {
    @Test void parsesCreationPoliciesAndRejectsUnknownValues() {
        for(var mode:TerrainSubmission.values()) assertSame(mode,TerrainSubmission.fromProperty(mode.propertyValue()));
        assertEquals(0,TerrainSubmission.AUTO.flags()); assertEquals(1,TerrainSubmission.DIRECT.flags());
        assertEquals(2,TerrainSubmission.INDIRECT.flags());
        assertThrows(IllegalArgumentException.class,()->TerrainSubmission.fromProperty("unknown"));
        assertThrows(IllegalArgumentException.class,()->TerrainSubmission.fromProperty("INDIRECT"));
    }
}
