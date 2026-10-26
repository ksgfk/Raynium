package dev.raynium.bridge;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;
import java.util.Optional;
import net.fabricmc.loader.api.ModContainer;

public final class NativeLibraryLocator {
    public record Payload(Path libraryPath, NativeManifest manifest, String source) {}

    private final Path gameDirectory;
    private final ModContainer mod;

    public NativeLibraryLocator(Path gameDirectory, ModContainer mod) {
        this.gameDirectory = gameDirectory;
        this.mod = mod;
    }

    public Optional<Payload> locate() throws IOException {
        String development = System.getProperty("raynium.native.dir");
        Path sourceDirectory;
        String source;
        if (development != null) {
            if (development.isBlank()) throw new IOException("raynium.native.dir is empty");
            sourceDirectory = Path.of(development).toAbsolutePath().normalize();
            source = "development directory " + sourceDirectory;
            if (!Files.isRegularFile(sourceDirectory.resolve("native-manifest.json"))) {
                Path pointer = sourceDirectory.resolve("current.json");
                JsonObject current = JsonParser.parseString(new String(readSmallFile(pointer), StandardCharsets.UTF_8)).getAsJsonObject();
                String content = current.get("contentId").getAsString();
                if (current.get("schemaVersion").getAsInt() != 1 || !content.matches("[0-9a-f]{64}")) {
                    throw new IOException("Invalid native stage pointer: " + pointer);
                }
                sourceDirectory = sourceDirectory.resolve(content);
                if (!sha256(sourceDirectory.resolve("native-manifest.json")).equals(content)) {
                    throw new IOException("Native stage manifest does not match current.json");
                }
            }
        } else {
            Optional<Path> embedded = mod.findPath("natives/windows-x86_64/native-manifest.json");
            if (embedded.isEmpty()) return Optional.empty();
            sourceDirectory = embedded.get().getParent();
            source = "Raynium mod JAR";
        }
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        if (!os.startsWith("windows") || !(arch.equals("amd64") || arch.equals("x86_64"))) {
            throw new IOException("Raynium native requires a Windows x86_64 JVM; found " + os + "/" + arch);
        }
        byte[] manifestBytes = readSmallFile(sourceDirectory.resolve("native-manifest.json"));
        NativeManifest manifest = NativeManifest.parse(manifestBytes);
        String content = HexFormat.of().formatHex(newDigest().digest(manifestBytes));
        Path cacheRoot = gameDirectory.resolve(".raynium/natives/windows-x86_64").toAbsolutePath().normalize();
        Files.createDirectories(cacheRoot);
        Path destination = cacheRoot.resolve(content);
        Path lockPath = cacheRoot.resolve(content + ".lock");
        try (FileChannel channel = FileChannel.open(lockPath, StandardOpenOption.CREATE, StandardOpenOption.WRITE);
             var lock = channel.lock()) {
            if (Files.exists(destination)) {
                verifyCache(destination, manifest, content);
            } else {
                Path temporary = Files.createTempDirectory(cacheRoot, ".extract-");
                try {
                    copyLibrary(sourceDirectory.resolve(manifest.library().path()), temporary.resolve(manifest.library().path()), manifest.library());
                    Files.write(temporary.resolve("native-manifest.json"), manifestBytes, StandardOpenOption.CREATE_NEW);
                    verifyCache(temporary, manifest, content);
                    try {
                        Files.move(temporary, destination, StandardCopyOption.ATOMIC_MOVE);
                    } catch (AtomicMoveNotSupportedException unsupported) {
                        Files.move(temporary, destination);
                    }
                } finally {
                    // Only these two files are ever created in this uniquely owned staging directory.
                    if (Files.exists(temporary, LinkOption.NOFOLLOW_LINKS)) {
                        Files.deleteIfExists(temporary.resolve(manifest.library().path()));
                        Files.deleteIfExists(temporary.resolve("native-manifest.json"));
                        Files.delete(temporary);
                    }
                }
            }
        }
        return Optional.of(new Payload(destination.resolve(manifest.library().path()), manifest, source));
    }

    private void verifyCache(Path directory, NativeManifest manifest, String content) throws IOException {
        Path file = directory.resolve(manifest.library().path());
        if (Files.isSymbolicLink(directory) || !Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS)
                || Files.size(file) != manifest.library().size() || !sha256(file).equals(manifest.library().sha256())
                || !sha256(directory.resolve("native-manifest.json")).equals(content)) {
            throw new IOException("Native cache is incomplete or changed: " + directory + "; remove it only after all clients have stopped");
        }
    }

    private void copyLibrary(Path source, Path destination, NativeManifest.FileEntry entry) throws IOException {
        try (InputStream input = Files.newInputStream(source);
             OutputStream output = Files.newOutputStream(destination, StandardOpenOption.CREATE_NEW)) {
            byte[] buffer = new byte[64 * 1024];
            long count = 0;
            int read;
            while ((read = input.read(buffer)) != -1) {
                count += read;
                if (count > entry.size()) throw new IOException("Native DLL is larger than its manifest: " + source);
                output.write(buffer, 0, read);
            }
            if (count != entry.size()) throw new IOException("Native DLL size mismatch: " + source);
        }
    }

    private byte[] readSmallFile(Path path) throws IOException {
        try (InputStream input = Files.newInputStream(path)) {
            byte[] bytes = input.readNBytes(1024 * 1024 + 1);
            if (bytes.length > 1024 * 1024) throw new IOException("Native metadata exceeds 1 MiB: " + path);
            return bytes;
        }
    }

    private String sha256(Path path) throws IOException {
        MessageDigest checksum = newDigest();
        try (InputStream input = Files.newInputStream(path)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = input.read(buffer)) != -1) checksum.update(buffer, 0, read);
        }
        return HexFormat.of().formatHex(checksum.digest());
    }

    private MessageDigest newDigest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException unavailable) {
            throw new IllegalStateException("JVM does not provide SHA-256", unavailable);
        }
    }
}
