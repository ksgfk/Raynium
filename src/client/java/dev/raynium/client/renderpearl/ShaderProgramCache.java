package dev.raynium.client.renderpearl;

import com.google.gson.Gson;
import com.google.gson.JsonParser;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.pipeline.ShaderType;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.zip.ZipFile;

/** CPU-side shader artifacts. Only the render thread creates the corresponding native pipeline. */
final class ShaderProgramCache {
    record Artifact(RayniumPrograms.Program program,byte[] pack) {}
    private final Path directory;
    private final ConcurrentHashMap<String,Artifact> compiled=new ConcurrentHashMap<>();

    ShaderProgramCache(Path gameDirectory) { directory=gameDirectory.resolve(".raynium/shader-cache/v2"); }

    Artifact resolve(String key,String vertex,String fragment,RenderPipeline pipeline) {
        return compiled.computeIfAbsent(key,k->loadOrCompile(k,vertex,fragment,pipeline));
    }

    private Artifact loadOrCompile(String key,String vertex,String fragment,RenderPipeline pipeline) {
        Path path=directory.resolve(key+".zip");
        String tools=System.getProperty("raynium.shaderToolsDir");
        if(Files.isRegularFile(path)) {
            try { return read(path,key); }
            catch(IOException | RuntimeException corrupt) {
                if(tools==null || tools.isBlank()) throw new IllegalStateException("Invalid cached shader "+path,corrupt);
            }
        }
        if(tools==null || tools.isBlank()) throw new IllegalArgumentException("Missing compiled program for "+pipeline.getLocation()+" ("+key+")");
        try {
            Files.createDirectories(directory);
            Path work=Files.createTempDirectory(directory,"compile-");
            Path request=work.resolve("request.json"), output=work.resolve("program.zip"), log=work.resolve("compiler.log");
            try {
                var stages=pipeline.getShaders();
                var defines=pipeline.getShaderDefines();
                var uniforms=com.mojang.renderpearl.api.pipeline.BindGroupLayout.flattenUniforms(pipeline.getBindGroupLayouts()).stream()
                    .filter(u->u.gpuFormat()!=null)
                    .map(u->Map.of("name",u.name(),"type",u.type().name(),"gpuFormat",u.gpuFormat().name())).toList();
                var identity=Map.of("shaders",Map.of("VERTEX",stages.get(ShaderType.VERTEX).toString(),"FRAGMENT",stages.get(ShaderType.FRAGMENT).toString()),
                    "defineFlags",defines.flags(),"defineValues",defines.values(),"uniforms",uniforms);
                Files.writeString(request,new Gson().toJson(Map.of("key",key,"identity",identity,"expanded",Map.of("VERTEX",vertex,"FRAGMENT",fragment))));
                Path script=Path.of(tools).toAbsolutePath().resolve("compile_shader_override.py");
                var process=new ProcessBuilder("python","-B",script.toString(),request.toAbsolutePath().toString(),output.toAbsolutePath().toString())
                    .redirectErrorStream(true).redirectOutput(log.toFile()).start();
                try {
                    if(process.waitFor()!=0) throw new IOException("Shader compilation failed for "+key+"\n"+Files.readString(log));
                } catch(InterruptedException interrupted) {
                    process.destroyForcibly();
                    Thread.currentThread().interrupt();
                    throw new IOException("Shader compilation interrupted",interrupted);
                }
                Artifact artifact=read(output,key);
                try { Files.move(output,path,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING); }
                catch(FileAlreadyExistsException race) { return read(path,key); }
                return artifact;
            } finally {
                Files.deleteIfExists(request); Files.deleteIfExists(output); Files.deleteIfExists(log); Files.deleteIfExists(work);
            }
        } catch(IOException failure) { throw new UncheckedIOException(failure); }
    }

    private static Artifact read(Path path,String key) throws IOException {
        try(var zip=new ZipFile(path.toFile())) {
            var metadata=zip.getEntry("program.json");
            var binary=zip.getEntry("program.rnsp");
            if(zip.size()!=2 || metadata==null || binary==null || metadata.getSize()<1 || metadata.getSize()>1<<20 ||
                    binary.getSize()<24 || binary.getSize()>64L<<20) throw new IOException("Invalid program archive");
            var json=JsonParser.parseString(new String(zip.getInputStream(metadata).readAllBytes(),StandardCharsets.UTF_8)).getAsJsonObject();
            byte[] bytes=zip.getInputStream(binary).readAllBytes();
            if(!key.equals(json.get("key").getAsString()) || !key.substring(0,58).equals(json.get("program").getAsString()) ||
                !HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes)).equals(json.get("packHash").getAsString()))
                throw new IOException("Shader cache identity mismatch");
            return new Artifact(new Gson().fromJson(json,RayniumPrograms.Program.class),bytes);
        } catch(NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }
}
