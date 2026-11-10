package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeShaderPack;
import java.io.IOException;
import java.io.UncheckedIOException;
import net.fabricmc.fabric.api.resource.v1.pack.ModPackResources;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.resources.ResourceManager;

public final class BuiltinShaderPack {
    public static final Identifier DEFAULT = Identifier.fromNamespaceAndPath("raynium", "shaderpacks/default.rnsp");
    private BuiltinShaderPack() {}

    public static NativeShaderPack load(ResourceManager manager) {
        try (var packs = manager.listPacks()) {
            var own = packs.filter(pack -> pack instanceof ModPackResources mod
                && mod.getFabricModMetadata().getId().equals("raynium")).toList();
            for (var pack : own) {
                var supplier = pack.getResource(PackType.CLIENT_RESOURCES, DEFAULT);
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
