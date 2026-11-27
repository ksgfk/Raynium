package dev.raynium.client.config;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;
import com.google.gson.stream.JsonReader;
import com.google.gson.stream.JsonToken;
import com.google.gson.stream.MalformedJsonException;
import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeRenderer;
import java.io.EOFException;
import java.io.IOException;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.Objects;
import java.util.Optional;
import java.util.function.Function;

/** Immutable client configuration loaded from <gameDir>/config/raynium.json with in-memory -Draynium.* overrides. */
public record RayniumConfig(
    NativeEnvironment.Backend backend,
    boolean rhiValidation,
    NativeRenderer.SubmissionMode terrainSubmission,
    Optional<Path> shaderToolsDir) {
    public static final RayniumConfig DEFAULT = new RayniumConfig(
        NativeEnvironment.Backend.D3D12,
        false,
        NativeRenderer.SubmissionMode.AUTO,
        Optional.empty());

    public static final String PROPERTY_BACKEND = "raynium.rhi.backend";
    public static final String PROPERTY_VALIDATION = "raynium.rhi.validation";
    public static final String PROPERTY_TERRAIN_SUBMISSION = "raynium.terrain.submission";
    public static final String PROPERTY_SHADER_TOOLS_DIR = "raynium.shader.tools-dir";

    private static final Gson PRETTY_GSON = new GsonBuilder().setPrettyPrinting().create();

    public RayniumConfig {
        Objects.requireNonNull(backend, "backend");
        Objects.requireNonNull(terrainSubmission, "terrainSubmission");
        Objects.requireNonNull(shaderToolsDir, "shaderToolsDir");
    }

    public static Path configPath(Path gameDirectory) {
        return Objects.requireNonNull(gameDirectory, "gameDirectory").resolve("config").resolve("raynium.json");
    }

    public static RayniumConfig load(Path gameDirectory) throws IOException {
        return load(gameDirectory, System::getProperty);
    }

    public static RayniumConfig load(Path gameDirectory, Function<String, String> properties) throws IOException {
        Objects.requireNonNull(properties, "properties");
        Path path = configPath(gameDirectory);
        NativeEnvironment.Backend backend = DEFAULT.backend();
        boolean rhiValidation = DEFAULT.rhiValidation();
        NativeRenderer.SubmissionMode terrainSubmission = DEFAULT.terrainSubmission();

        if (Files.notExists(path)) {
            writeDefault(path);
        } else {
            String content = Files.readString(path, StandardCharsets.UTF_8);
            var parsed = parseFile(content, path);
            backend = parsed.backend();
            rhiValidation = parsed.rhiValidation();
            terrainSubmission = parsed.terrainSubmission();
        }

        String backendOverride = properties.apply(PROPERTY_BACKEND);
        if (backendOverride != null) {
            backend = NativeEnvironment.Backend.fromProperty(backendOverride);
        }
        String validationOverride = properties.apply(PROPERTY_VALIDATION);
        if (validationOverride != null) {
            rhiValidation = parseStrictBoolean(validationOverride, PROPERTY_VALIDATION);
        }
        String submissionOverride = properties.apply(PROPERTY_TERRAIN_SUBMISSION);
        if (submissionOverride != null) {
            terrainSubmission = NativeRenderer.SubmissionMode.fromProperty(submissionOverride);
        }
        Optional<Path> shaderToolsDir = Optional.empty();
        String toolsOverride = properties.apply(PROPERTY_SHADER_TOOLS_DIR);
        if (toolsOverride != null) {
            if (toolsOverride.isBlank()) {
                throw new IllegalArgumentException("Invalid " + PROPERTY_SHADER_TOOLS_DIR + ": must not be blank");
            }
            shaderToolsDir = Optional.of(Path.of(toolsOverride).toAbsolutePath().normalize());
        }
        return new RayniumConfig(backend, rhiValidation, terrainSubmission, shaderToolsDir);
    }

    private static void writeDefault(Path path) throws IOException {
        Path parent = path.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        var root = new JsonObject();
        root.addProperty("backend", DEFAULT.backend().propertyValue());
        root.addProperty("rhiValidation", DEFAULT.rhiValidation());
        root.addProperty("terrainSubmission", DEFAULT.terrainSubmission().propertyValue());
        Files.writeString(path, PRETTY_GSON.toJson(root) + "\n", StandardCharsets.UTF_8);
    }

    private static RayniumConfig parseFile(String content, Path path) {
        NativeEnvironment.Backend backend = DEFAULT.backend();
        boolean rhiValidation = DEFAULT.rhiValidation();
        NativeRenderer.SubmissionMode terrainSubmission = DEFAULT.terrainSubmission();
        var seen = new HashSet<String>(4);
        try (var reader = new JsonReader(new StringReader(content))) {
            if (reader.peek() != JsonToken.BEGIN_OBJECT) {
                throw new IllegalArgumentException("Raynium config must be a JSON object: " + path);
            }
            reader.beginObject();
            while (reader.hasNext()) {
                String name = reader.nextName();
                if (!seen.add(name)) {
                    throw new IllegalArgumentException("Duplicate field in raynium.json: '" + name + "'");
                }
                switch (name) {
                    case "backend" -> {
                        if (reader.peek() != JsonToken.STRING) {
                            throw new IllegalArgumentException("Field 'backend' in raynium.json must be a string");
                        }
                        backend = NativeEnvironment.Backend.fromProperty(reader.nextString());
                    }
                    case "rhiValidation" -> {
                        if (reader.peek() != JsonToken.BOOLEAN) {
                            throw new IllegalArgumentException("Field 'rhiValidation' in raynium.json must be a boolean");
                        }
                        rhiValidation = reader.nextBoolean();
                    }
                    case "terrainSubmission" -> {
                        if (reader.peek() != JsonToken.STRING) {
                            throw new IllegalArgumentException("Field 'terrainSubmission' in raynium.json must be a string");
                        }
                        terrainSubmission = NativeRenderer.SubmissionMode.fromProperty(reader.nextString());
                    }
                    default -> throw new IllegalArgumentException("Unknown field in raynium.json: '" + name + "'");
                }
            }
            reader.endObject();
            if (reader.peek() != JsonToken.END_DOCUMENT) {
                throw new IllegalArgumentException("Unexpected trailing tokens in raynium.json: " + path);
            }
        } catch (MalformedJsonException | EOFException | IllegalStateException failure) {
            throw new IllegalArgumentException("Malformed Raynium config JSON: " + path, failure);
        } catch (IOException failure) {
            throw new IllegalStateException("Failed to read Raynium config string: " + path, failure);
        }
        return new RayniumConfig(backend, rhiValidation, terrainSubmission, Optional.empty());
    }

    private static boolean parseStrictBoolean(String value, String propertyName) {
        if ("true".equals(value)) {
            return true;
        }
        if ("false".equals(value)) {
            return false;
        }
        throw new IllegalArgumentException("Invalid " + propertyName + "='" + value + "'; expected true or false");
    }
}
