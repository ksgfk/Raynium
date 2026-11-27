package dev.raynium.client.config;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeRenderer;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class RayniumConfigTest {
    @TempDir
    Path gameDir;

    @Test
    void createsDefaultConfigFileWhenMissing() throws IOException {
        RayniumConfig config = RayniumConfig.load(gameDir, key -> null);
        assertEquals(RayniumConfig.DEFAULT, config);
        Path file = RayniumConfig.configPath(gameDir);
        assertTrue(Files.isRegularFile(file));
        String written = Files.readString(file, StandardCharsets.UTF_8);
        assertTrue(written.contains("\"backend\": \"d3d12\""));
        assertTrue(written.contains("\"rhiValidation\": false"));
        assertTrue(written.contains("\"terrainSubmission\": \"auto\""));
        assertEquals(config, RayniumConfig.load(gameDir, key -> null));
    }

    @Test
    void missingFieldsDefaultAndOverridesDoNotWriteBack() throws IOException {
        Path file = RayniumConfig.configPath(gameDir);
        Files.createDirectories(file.getParent());
        String original = "{\"backend\": \"vulkan\"}\n";
        Files.writeString(file, original, StandardCharsets.UTF_8);

        RayniumConfig fromFile = RayniumConfig.load(gameDir, key -> null);
        assertEquals(NativeEnvironment.Backend.VULKAN, fromFile.backend());
        assertFalse(fromFile.rhiValidation());
        assertEquals(NativeRenderer.SubmissionMode.AUTO, fromFile.terrainSubmission());
        assertEquals(Optional.empty(), fromFile.shaderToolsDir());

        Path toolsDir = gameDir.resolve("tools");
        Map<String, String> props = Map.of(
            RayniumConfig.PROPERTY_BACKEND, "d3d12",
            RayniumConfig.PROPERTY_VALIDATION, "true",
            RayniumConfig.PROPERTY_TERRAIN_SUBMISSION, "indirect",
            RayniumConfig.PROPERTY_SHADER_TOOLS_DIR, toolsDir.toString());
        RayniumConfig overridden = RayniumConfig.load(gameDir, props::get);
        assertEquals(NativeEnvironment.Backend.D3D12, overridden.backend());
        assertTrue(overridden.rhiValidation());
        assertEquals(NativeRenderer.SubmissionMode.INDIRECT, overridden.terrainSubmission());
        assertEquals(Optional.of(toolsDir.toAbsolutePath().normalize()), overridden.shaderToolsDir());
        assertEquals(original, Files.readString(file, StandardCharsets.UTF_8));
    }

    @Test
    void failsFastOnUnknownFieldsWrongTypesInvalidEnumsAndMalformedJson() throws IOException {
        Path file = RayniumConfig.configPath(gameDir);
        Files.createDirectories(file.getParent());

        for (String invalid : new String[] {
                 "{\"unknown\": true}",
                 "{\"backend\": \"vulkn\"}",
                 "{\"backend\": 123}",
                 "{\"backend\": null}",
                 "{\"rhiValidation\": \"true\"}",
                 "{\"terrainSubmission\": \"DIRECT\"}",
                 "{\"backend\": \"d3d12\", \"backend\": \"vulkan\"}",
                 "[]",
                 "{broken",
                 "{\"backend\": \"d3d12\"} trailing"}) {
            Files.writeString(file, invalid, StandardCharsets.UTF_8);
            assertThrows(IllegalArgumentException.class, () -> RayniumConfig.load(gameDir, key -> null), invalid);
        }
    }

    @Test
    void failsFastOnInvalidPropertyOverrides() throws IOException {
        assertThrows(IllegalArgumentException.class,
                     () -> RayniumConfig.load(gameDir, Map.of(RayniumConfig.PROPERTY_BACKEND, "opengl")::get));
        assertThrows(IllegalArgumentException.class,
                     () -> RayniumConfig.load(gameDir, Map.of(RayniumConfig.PROPERTY_VALIDATION, "yes")::get));
        assertThrows(IllegalArgumentException.class,
                     () -> RayniumConfig.load(gameDir, Map.of(RayniumConfig.PROPERTY_TERRAIN_SUBMISSION, "fast")::get));
        assertThrows(IllegalArgumentException.class,
                     () -> RayniumConfig.load(gameDir, Map.of(RayniumConfig.PROPERTY_SHADER_TOOLS_DIR, "   ")::get));
    }
}
