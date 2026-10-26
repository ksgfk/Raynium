package dev.raynium.bridge;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class NativeRuntimeBackendTest {
    @ParameterizedTest
    @EnumSource(NativeRuntime.Backend.class)
    void parsesBackendProperty(NativeRuntime.Backend backend) {
        assertEquals(backend, NativeRuntime.Backend.fromProperty(backend.propertyValue()));
    }

    @Test
    void rejectsUnknownBackend() {
        var failure = assertThrows(IllegalArgumentException.class,
                () -> NativeRuntime.Backend.fromProperty("invalid"));
        assertTrue(failure.getMessage().contains("raynium.rhi.backend"));
    }
}
