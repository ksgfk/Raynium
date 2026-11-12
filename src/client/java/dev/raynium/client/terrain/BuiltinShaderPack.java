package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeShaderPack;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.stream.Stream;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.server.packs.resources.IoSupplier;

public final class BuiltinShaderPack {
    public static final String DEFAULT = "assets/raynium/shaderpacks/default.rnsp";
    record Source(String modId,IoSupplier<InputStream> input) {}
    private BuiltinShaderPack() {}

    public static NativeShaderPack load() {
        return load(FabricLoader.getInstance().getAllMods().stream().map(mod -> new Source(mod.getMetadata().getId(),
            () -> Files.newInputStream(mod.findPath(DEFAULT).orElseThrow(
                () -> new IllegalStateException("Missing Raynium built-in shader pack " + DEFAULT))))));
    }
    static NativeShaderPack load(Stream<Source> sources) {
        try (sources) {
            for (var source : sources.filter(entry -> entry.modId().equals("raynium")).toList()) {
                var supplier = source.input();
                if (supplier == null) continue;
                try (var input = supplier.get()) {
                    return NativeShaderPack.read(input);
                } catch (IOException failure) {
                    throw new UncheckedIOException("Reading Raynium built-in shader pack " + DEFAULT, failure);
                }
            }
        }
        throw new IllegalStateException("Missing Raynium built-in shader pack " + DEFAULT);
    }
}
