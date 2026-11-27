package dev.raynium.tools;

import com.google.gson.GsonBuilder;
import dev.raynium.client.renderpearl.VanillaPipelineCatalog;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;

/** Development inventory from the locked Minecraft classes, including unregistered same-name variants. */
public final class ExportVanillaPipelines {
    public static void main(String[] arguments) throws Exception {
        if(arguments.length!=1) throw new IllegalArgumentException("Expected output path");
        var entries=VanillaPipelineCatalog.create().entries().stream().map(entry-> {
            var value=LinkedHashMap.<String,Object>newLinkedHashMap(5);
            value.put("id",entry.id()); value.put("names",entry.names());
            value.put("location",entry.pipeline().getLocation().toString());
            value.put("optional",entry.optional()); value.put("identity",entry.identity());
            return value;
        }).toList();
        Path output=Path.of(arguments[0]); Files.createDirectories(output.toAbsolutePath().getParent());
        var gson=new GsonBuilder().setPrettyPrinting().serializeNulls()
            .registerTypeHierarchyAdapter(java.util.Optional.class,
                (com.google.gson.JsonSerializer<java.util.Optional<?>>)(value,type,context)->context.serialize(value.orElse(null)))
            .registerTypeAdapter(net.minecraft.resources.Identifier.class,
                (com.google.gson.JsonSerializer<net.minecraft.resources.Identifier>)(value,type,context)->new com.google.gson.JsonPrimitive(value.toString()))
            .create();
        Files.writeString(output,gson.toJson(entries));
        System.out.println("Exported "+entries.size()+" vanilla pipeline variants to "+output);
    }
}
