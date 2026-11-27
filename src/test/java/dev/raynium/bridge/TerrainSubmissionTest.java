package dev.raynium.bridge;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

class TerrainSubmissionTest {
    @Test
    void parsesCreationPoliciesAndRejectsUnknownValues() {
        for (var mode : NativeRenderer.SubmissionMode.values())
            assertSame(mode, NativeRenderer.SubmissionMode.fromProperty(mode.propertyValue()));
        assertEquals(0, NativeRenderer.SubmissionMode.AUTO.flags());
        assertEquals(1, NativeRenderer.SubmissionMode.DIRECT.flags());
        assertEquals(2, NativeRenderer.SubmissionMode.INDIRECT.flags());
        assertThrows(IllegalArgumentException.class, () -> NativeRenderer.SubmissionMode.fromProperty("unknown"));
        assertThrows(IllegalArgumentException.class, () -> NativeRenderer.SubmissionMode.fromProperty("INDIRECT"));
    }
}
