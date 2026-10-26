package dev.raynium.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.nio.charset.StandardCharsets;

public record NativeManifest(int abiVersion, int abiInfoSize, String buildId, String radrayCommit,
                             boolean radrayDirty, String configuration, String platform,
                             String toolchain, FileEntry library) {
    public static final int ABI_VERSION = 3;
    public record FileEntry(String path, long size, String sha256) {}

    public static NativeManifest parse(byte[] bytes) {
        JsonObject object = JsonParser.parseString(new String(bytes, StandardCharsets.UTF_8)).getAsJsonObject();
        if (object.get("schemaVersion").getAsInt() != 1 || object.get("abiVersion").getAsInt() != ABI_VERSION
                || object.get("abiInfoSize").getAsInt() != 240 || !"MT".equals(object.get("crt").getAsString())) {
            throw new IllegalArgumentException("Unsupported Raynium native manifest schema, ABI or CRT");
        }
        String platform = object.get("platform").getAsString();
        String configuration = object.get("configuration").getAsString();
        String buildId = object.get("buildId").getAsString();
        String commit = object.get("radrayCommit").getAsString();
        String toolchain = object.get("toolchain").getAsString();
        if (!platform.equals("windows-x86_64") || !(configuration.equals("Debug") || configuration.equals("Release"))
                || !buildId.matches("[0-9a-f]{64}") || !commit.matches("[0-9a-f]{40}") || toolchain.isBlank()
                || !object.get("radrayDirty").getAsJsonPrimitive().isBoolean()) {
            throw new IllegalArgumentException("Invalid Raynium native build identity");
        }
        JsonArray files = object.getAsJsonArray("files");
        if (files.size() != 1) throw new IllegalArgumentException("Unsupported native payload file list");
        JsonObject file = files.get(0).getAsJsonObject();
        FileEntry library = new FileEntry(file.get("path").getAsString(), file.get("size").getAsLong(),
                file.get("sha256").getAsString());
        if (!library.path().equals("rayniumruntime.dll") || library.size() <= 0 || library.size() > 1024L * 1024 * 1024
                || !library.sha256().matches("[0-9a-f]{64}")) {
            throw new IllegalArgumentException("Invalid Raynium native DLL entry");
        }
        return new NativeManifest(ABI_VERSION, 240, buildId, commit, object.get("radrayDirty").getAsBoolean(),
                configuration, platform, toolchain, library);
    }
}
