package dev.raynium.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import static org.junit.jupiter.api.Assertions.*;

class NativeManifestTest {
    private JsonObject manifest(String configuration) {
        JsonObject object = new JsonObject();
        object.addProperty("schemaVersion", 1);
        object.addProperty("abiVersion", NativeManifest.ABI_VERSION);
        object.addProperty("abiInfoSize", 240);
        object.addProperty("platform", "windows-x86_64");
        object.addProperty("configuration", configuration);
        object.addProperty("crt", configuration.equals("Debug") ? "MTd" : "MT");
        object.addProperty("buildId", "a".repeat(64));
        object.addProperty("radrayCommit", "b".repeat(40));
        object.addProperty("radrayDirty", false);
        object.addProperty("toolchain", "MSVC-test");
        JsonArray files = new JsonArray();
        for (String name : new String[]{"rayniumruntime.dll"}) {
            JsonObject entry = new JsonObject();
            entry.addProperty("path", name);
            entry.addProperty("size", 1);
            entry.addProperty("sha256", "c".repeat(64));
            files.add(entry);
        }
        object.add("files", files);
        return object;
    }

    private NativeManifest parse(JsonObject object) {
        return NativeManifest.parse(object.toString().getBytes(StandardCharsets.UTF_8));
    }

    @ParameterizedTest @ValueSource(strings = {"Debug", "Release"})
    void acceptsMatchingCrtAndCompletePayload(String configuration) {
        var parsed = parse(manifest(configuration));
        assertEquals("rayniumruntime.dll", parsed.library().path());
        assertEquals(configuration.equals("Debug") ? "MTd" : "MT", parsed.crt());
    }

    @ParameterizedTest @ValueSource(strings = {"schema", "old-abi", "MD", "MDd", "MTd", "debug-mt", "missing", "extra", "path", "hash"})
    void rejectsIncompatibleOrUnsafePayload(String mutation) {
        var object = manifest("Release");
        var files = object.getAsJsonArray("files");
        switch (mutation) {
            case "old-abi" -> object.addProperty("abiVersion", 10);
            case "schema" -> object.addProperty("schemaVersion", 2);
            case "MD", "MDd", "MTd" -> object.addProperty("crt", mutation);
            case "debug-mt" -> object.addProperty("configuration", "Debug");
            case "missing" -> files.remove(0);
            case "path" -> files.get(0).getAsJsonObject().addProperty("path", "../rayniumruntime.dll");
            case "extra" -> files.add(files.get(0).deepCopy());
            case "hash" -> files.get(0).getAsJsonObject().addProperty("sha256", "invalid");
            default -> throw new AssertionError(mutation);
        }
        assertThrows(IllegalArgumentException.class, () -> parse(object));
    }
}
