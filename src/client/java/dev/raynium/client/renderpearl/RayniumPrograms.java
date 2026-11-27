package dev.raynium.client.renderpearl;

import com.google.gson.Gson;
import com.google.gson.reflect.TypeToken;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.pipeline.BindGroupLayout;
import com.mojang.renderpearl.api.pipeline.BlendFactor;
import com.mojang.renderpearl.api.pipeline.PolygonMode;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.pipeline.ShaderSource;
import com.mojang.renderpearl.api.pipeline.ShaderType;
import com.mojang.renderpearl.api.pipeline.UniformType;
import dev.raynium.bridge.NativeTexture;
import it.unimi.dsi.fastutil.ints.IntArrayList;
import it.unimi.dsi.fastutil.ints.IntList;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import net.minecraft.client.renderer.ShaderDefines;
import net.minecraft.resources.Identifier;
import org.lwjgl.system.MemoryUtil;
import org.lwjgl.util.shaderc.ShadercIncludeResult;
import org.slf4j.LoggerFactory;
final class RayniumPrograms {
    record Input(int location, String type) {}
    record Resource(int group, int binding, String type, int sampler, String texelFormat) {}
    record InstancedProgram(String mode, String program, Map<String, Input> inputs, Map<String, Resource> resources, int pushBytes) {}
    record Program(String program, Map<String, Input> inputs, Map<String, Resource> resources, int pushBytes, InstancedProgram instanced) {}
    record Route(int group, int binding, int sampler, UniformType type, GpuFormat format, boolean cube) {}
    record InstancedPreparation(String mode, String name, int[] words, List<Route> routes, List<String> uniforms) {}
    record Preparation(String name, int[] words, List<Route> routes, List<String> uniforms, byte[] pack, InstancedPreparation instanced) {}
    private static final Pattern INCLUDE = Pattern.compile("(?m)^\\s*#include\\s+<([^>]+)>\\s*$");
    private final Map<String, Program> programs;

    private final Map<String, String> terrainKeys;
    private final AtomicBoolean warnedTerrain = new AtomicBoolean();
    private final ShaderProgramCache cache;

    RayniumPrograms(Path gameDirectory, Optional<Path> shaderToolsDir) {
        cache = new ShaderProgramCache(gameDirectory, shaderToolsDir);
        try (var stream = Objects.requireNonNull(RayniumPrograms.class.getResourceAsStream("/assets/raynium/shaderpacks/terrain-programs.json"));
             var reader = new InputStreamReader(stream, StandardCharsets.UTF_8)) {
            terrainKeys = Map.copyOf(new Gson().fromJson(reader, new TypeToken<Map<String, String>>() {}));
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        var stream = RayniumPrograms.class.getResourceAsStream("/assets/raynium/shaderpacks/default.json");
        if (stream == null)
            throw new IllegalStateException("Missing RenderPearl program catalog");
        try (var reader = new InputStreamReader(stream, StandardCharsets.UTF_8)) {
            programs = Map.copyOf(new Gson().fromJson(reader, new TypeToken<Map<String, Program>>() {}));
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    static String expand(String source, ShaderSource shaders, Set<Identifier> stack) {
        if (source == null)
            throw new IllegalArgumentException("Missing shader source");
        return INCLUDE.matcher(source.replace("\r\n", "\n").replace('\r', '\n')).replaceAll(match -> {
            var id = Identifier.parse(match.group(1));
            if (!stack.add(id))
                throw new IllegalArgumentException("Recursive shader include " + id);
            try {
                var entry = shaders.getInclude(id);
                if (entry == null)
                    throw new IllegalArgumentException("Missing shader include " + id);
                var include = ShadercIncludeResult.create(entry.includeResultPtr());
                long address = MemoryUtil.memGetAddress(include.address() + ShadercIncludeResult.CONTENT);
                var bytes = MemoryUtil.memByteBuffer(address, Math.toIntExact(include.content_length()));
                return Matcher.quoteReplacement(expand(StandardCharsets.UTF_8.decode(bytes).toString(), shaders, stack));
            } finally {
                stack.remove(id);
            }
        });
    }

    private static String digest(String input) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(input.getBytes(StandardCharsets.UTF_8)));
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static List<String> defineFields(ShaderDefines defines) {
        var sorted = new TreeMap<String, String>();
        defines.flags().forEach(flag -> sorted.put(flag, ""));
        sorted.putAll(defines.values());
        var fields = new ArrayList<String>(2 * sorted.size());
        sorted.forEach((name, value) -> { fields.add(name); fields.add(value); });
        return fields;
    }

    static String key(String vertex, String fragment, ShaderDefines defines) {
        var definitions = defineFields(defines);
        var fields = new ArrayList<String>(2 + definitions.size());
        fields.add(digest(vertex));
        fields.add(digest(fragment));
        fields.addAll(definitions);
        return "p_" + digest(String.join("\0", fields));
    }

    Preparation prepare(RenderPipeline pipeline, ShaderSource source) {
        var stages = pipeline.getShaders();
        var defines = pipeline.getShaderDefines();
        var terrain = Identifier.parse("minecraft:core/terrain");
        String fallback = terrain.equals(stages.get(ShaderType.VERTEX)) && terrain.equals(stages.get(ShaderType.FRAGMENT))
                              ? terrainKeys.get(digest(String.join("\0", defineFields(defines))))
                              : null;
        String vertex, fragment;
        try {
            vertex = expand(source.getShader(stages.get(ShaderType.VERTEX), ShaderType.VERTEX), source, new HashSet<>());
            fragment = expand(source.getShader(stages.get(ShaderType.FRAGMENT), ShaderType.FRAGMENT), source, new HashSet<>());
        } catch (IllegalArgumentException error) {
            if (fallback != null)
                return retained(pipeline, fallback);
            throw error;
        }
        String key = key(vertex, fragment, defines);
        if (fallback != null && !fallback.equals(key))
            return retained(pipeline, fallback);
        var builtin = programs.get(key);
        if (builtin != null)
            return prepare(pipeline, builtin, null);
        var external = cache.resolve(key, vertex, fragment, pipeline);
        return prepare(pipeline, external.program(), external.pack());
    }

    private Preparation retained(RenderPipeline pipeline, String key) {
        if (warnedTerrain.compareAndSet(false, true))
            LoggerFactory.getLogger("raynium").warn("Retained terrain uses the built-in shader; the resource pack terrain override is ignored");
        return prepare(pipeline, Objects.requireNonNull(programs.get(key)), null);
    }

    static Preparation prepare(RenderPipeline pipeline, Program program, byte[] pack) {
        if (program.pushBytes() > pipeline.pushConstantSize())
            throw new IllegalArgumentException("Push constant range exceeds pipeline layout");
        var declared = BindGroupLayout.flattenUniforms(pipeline.getBindGroupLayouts());
        int resourceCount = program.resources().size();
        var routes = new ArrayList<Route>(resourceCount);
        var names = new ArrayList<String>(resourceCount);
        resolveResources(declared, program.resources(), routes, names);
        var formats = pipeline.getVertexFormatBindings();
        int bindingCount = 0;
        for (var format : formats)
            if (format != null)
                ++bindingCount;
        var bindings = new IntArrayList(3 * bindingCount);
        var attributes = new IntArrayList(4 * program.inputs().size());
        var used = HashSet.<String>newHashSet(program.inputs().size());
        for (int slot = 0; slot < formats.size(); ++slot) {
            var format = formats.get(slot);
            if (format == null)
                continue;
            bindings.add(slot);
            bindings.add(format.getVertexSize());
            bindings.add(pipeline.getPrimitiveTopology() == PrimitiveTopology.POINTS && slot == 0 ? 1 : format.getStepRate());
            for (var element : format.getElements()) {
                var input = program.inputs().get(element.name());
                if (input == null)
                    continue;
                if (!used.add(element.name()))
                    throw new IllegalArgumentException("Duplicate active vertex element " + element.name());
                int value = vertexFormat(element.format(), input.type());
                attributes.add(slot);
                attributes.add(input.location());
                attributes.add(element.offset());
                attributes.add(value);
            }
        }
        if (used.size() != program.inputs().size())
            throw new IllegalArgumentException("Missing active vertex input");
        int[] words = encodeStateWords(pipeline, bindings, attributes);
        InstancedPreparation instanced = null;
        if (pack == null && program.instanced() != null && pipeline.getPrimitiveTopology() == PrimitiveTopology.QUADS) {
            instanced = prepareInstanced(pipeline, declared, program.instanced());
        }
        return new Preparation(program.program(), words, List.copyOf(routes), List.copyOf(names), pack, instanced);
    }

    private static void resolveResources(List<BindGroupLayout.UniformDescription> declared, Map<String, Resource> resources,
                                         List<Route> routes, List<String> names) {
        for (var entry : new TreeMap<>(resources).entrySet()) {
            var r = entry.getValue();
            var uniform = declared.stream().filter(u -> u.name().equals(entry.getKey())).findFirst().orElseThrow(() -> new IllegalArgumentException("Missing shader resource " + entry.getKey()));
            var type = switch (r.type().replaceFirst("^[iu]sampler", "sampler")) {
                case "ubo" -> UniformType.UNIFORM_BUFFER;
                case "sampler2D", "samplerCube", "sampler2DRect" -> UniformType.COMBINED_IMAGE_SAMPLER;
                case "samplerBuffer" -> UniformType.TEXEL_BUFFER;
                default -> throw new IllegalArgumentException("Unsupported shader resource type " + r.type());
            };
            if (uniform.type() != type)
                throw new IllegalArgumentException("Resource type mismatch for " + entry.getKey());
            if (type == UniformType.TEXEL_BUFFER) {
                NativeTexture.Format.from(Objects.requireNonNull(uniform.gpuFormat()));
                if (!uniform.gpuFormat().name().equals(r.texelFormat()))
                    throw new IllegalArgumentException("Texel buffer format mismatch for " + uniform.name());
            }
            names.add(uniform.name());
            routes.add(new Route(r.group(), r.binding(), type == UniformType.COMBINED_IMAGE_SAMPLER ? r.sampler() : -1, type, uniform.gpuFormat(), r.type().endsWith("Cube")));
        }
    }

    private record InstancedAttr(int slot, String name, int offset, GpuFormat format) {}
    private static final List<InstancedAttr> ENTITY_INSTANCED_ATTRS = List.of(
        new InstancedAttr(0, "LocalPos", 0, GpuFormat.RGB32_FLOAT),
        new InstancedAttr(0, "UV0", 12, GpuFormat.RG32_FLOAT),
        new InstancedAttr(0, "LocalNormal", 20, GpuFormat.RGBA8_SNORM),
        new InstancedAttr(1, "InstRow0", 0, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstRow1", 16, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstRow2", 32, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstColor", 48, GpuFormat.RGBA8_UNORM),
        new InstancedAttr(1, "InstUV1", 52, GpuFormat.RG16_SINT),
        new InstancedAttr(1, "InstUV2", 56, GpuFormat.RG16_SINT),
        new InstancedAttr(1, "InstNormalX", 60, GpuFormat.RGBA8_SNORM),
        new InstancedAttr(1, "InstNormalY", 64, GpuFormat.RGBA8_SNORM),
        new InstancedAttr(1, "InstNormalZ", 68, GpuFormat.RGBA8_SNORM));
    private static final List<InstancedAttr> PARTICLE_INSTANCED_ATTRS = List.of(
        new InstancedAttr(0, "CornerPos", 0, GpuFormat.RG32_FLOAT),
        new InstancedAttr(0, "CornerUvMask", 12, GpuFormat.RG32_FLOAT),
        new InstancedAttr(1, "InstPosScale", 0, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstRot", 16, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstUvRect", 32, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstColor", 48, GpuFormat.RGBA8_UNORM),
        new InstancedAttr(1, "InstUV2", 52, GpuFormat.RG16_SINT));

    private static final List<InstancedAttr> SHADOW_INSTANCED_ATTRS = List.of(
        new InstancedAttr(0, "LocalPos", 0, GpuFormat.RGB32_FLOAT),
        new InstancedAttr(1, "InstOriginU0", 0, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstEdgeXU1", 16, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstEdgeZV0", 32, GpuFormat.RGBA32_FLOAT),
        new InstancedAttr(1, "InstV1", 48, GpuFormat.R32_FLOAT),
        new InstancedAttr(1, "InstColor", 52, GpuFormat.RGBA8_UNORM));

    private static InstancedPreparation prepareInstanced(RenderPipeline pipeline, List<BindGroupLayout.UniformDescription> declared, InstancedProgram inst) {
        if (inst.pushBytes() > pipeline.pushConstantSize())
            return null;
        int stride0, stride1;
        List<InstancedAttr> spec;
        if ("entity".equals(inst.mode())) {
            stride0 = 24;
            stride1 = 80;
            spec = ENTITY_INSTANCED_ATTRS;
        } else if ("particle".equals(inst.mode())) {
            stride0 = 24;
            stride1 = 56;
            spec = PARTICLE_INSTANCED_ATTRS;
        } else if ("shadow".equals(inst.mode())) {
            stride0 = 24;
            stride1 = 56;
            spec = SHADOW_INSTANCED_ATTRS;
        } else
            return null;
        int resourceCount = inst.resources().size();
        var routes = new ArrayList<Route>(resourceCount);
        var names = new ArrayList<String>(resourceCount);
        resolveResources(declared, inst.resources(), routes, names);
        var bindings = IntList.of(0, stride0, 0, 1, stride1, 1);
        var attributes = new IntArrayList(4 * inst.inputs().size());
        for (var attr : spec) {
            var input = inst.inputs().get(attr.name());
            if (input == null)
                continue;
            attributes.add(attr.slot());
            attributes.add(input.location());
            attributes.add(attr.offset());
            attributes.add(vertexFormat(attr.format(), input.type()));
        }
        if (attributes.size() / 4 != inst.inputs().size())
            throw new IllegalArgumentException("Missing active instanced vertex input for " + inst.program());
        return new InstancedPreparation(inst.mode(), inst.program(), encodeStateWords(pipeline, bindings, attributes), List.copyOf(routes), List.copyOf(names));
    }

    private static int[] encodeStateWords(RenderPipeline pipeline, IntList bindings, IntList attributes) {
        int topology = switch (pipeline.getPrimitiveTopology()) {
            case DEBUG_LINES -> 1;
            case DEBUG_LINE_STRIP -> 2;
            case TRIANGLE_STRIP -> 3;
            default -> 0;
        };
        var depth = pipeline.getDepthStencilState();
        int compare = depth == null ? 8 : switch (depth.depthTest()) {
            case NEVER_PASS -> 0;
            case LESS_THAN -> 1;
            case EQUAL -> 2;
            case LESS_THAN_OR_EQUAL -> 3;
            case GREATER_THAN -> 4;
            case NOT_EQUAL -> 5;
            case GREATER_THAN_OR_EQUAL -> 6;
            case ALWAYS_PASS -> 7;
        };
        var colors = pipeline.getColorTargetStates();
        int[] words = new int[9 + bindings.size() + attributes.size() + 9 * colors.size()];
        words[0] = topology;
        words[1] = (pipeline.isCull() ? 1 : 0) | (pipeline.getPolygonMode() == PolygonMode.WIREFRAME ? 2 : 0);
        words[2] = compare;
        words[3] = depth != null && depth.writeDepth() ? 1 : 0;
        words[4] = depth == null ? 0 : Float.floatToIntBits(depth.depthBiasConstant());
        words[5] = depth == null ? 0 : Float.floatToIntBits(depth.depthBiasScaleFactor());
        words[6] = bindings.size() / 3;
        words[7] = attributes.size() / 4;
        words[8] = colors.size();
        int offset = 9;
        bindings.getElements(0, words, offset, bindings.size());
        offset += bindings.size();
        attributes.getElements(0, words, offset, attributes.size());
        offset += attributes.size();
        for (var color : colors) {
            if (color == null)
                throw new IllegalArgumentException("Unbound color target slot");
            var blend = color.blendFunction().orElse(null);
            words[offset++] = NativeTexture.Format.from(color.format()).ordinal();
            words[offset++] = color.writeMask();
            words[offset++] = blend == null ? 0 : 1;
            if (blend == null)
                offset += 6;
            else
                for (var equation : List.of(blend.color(), blend.alpha())) {
                    words[offset++] = blendFactor(equation.sourceFactor());
                    words[offset++] = blendFactor(equation.destFactor());
                    words[offset++] = equation.op().ordinal();
                }
        }
        return words;
    }

    private static int blendFactor(BlendFactor f) {
        return switch (f) {
            case ZERO -> 0;
            case ONE -> 1;
            case SRC_COLOR -> 2;
            case ONE_MINUS_SRC_COLOR -> 3;
            case SRC_ALPHA -> 4;
            case ONE_MINUS_SRC_ALPHA -> 5;
            case DST_COLOR -> 6;
            case ONE_MINUS_DST_COLOR -> 7;
            case DST_ALPHA -> 8;
            case ONE_MINUS_DST_ALPHA -> 9;
            case SRC_ALPHA_SATURATE -> 10;
            case CONSTANT_ALPHA, CONSTANT_COLOR -> 11;
            case ONE_MINUS_CONSTANT_ALPHA, ONE_MINUS_CONSTANT_COLOR -> 12;
        };
    }

    private static int vertexFormat(GpuFormat format, String type) {
        int c = format.componentCount();
        String base = switch (format.componentType()) {
            case UINT_8, UINT_16, UINT_32 -> "u";
            case SINT_8, SINT_16, SINT_32 -> "i";
            default -> "";
        };
        boolean compatible = false;
        for (int n = 1; n <= c; ++n)
            compatible |= type.equals(n == 1 ? (base.isEmpty() ? "float" : base.equals("i") ? "int"
                                                                                            : "uint")
                                             : base + "vec" + n);
        if (!compatible || (format.blockSize() / c != 4 && c != 2 && c != 4))
            throw new IllegalArgumentException("Vertex " + format + " cannot supply " + type);
        int start = switch (format.componentType()) {
            case UINT_8 -> 1;
            case SINT_8 -> 3;
            case UNORM_8 -> 5;
            case SNORM_8 -> 7;
            case UINT_16 -> 9;
            case SINT_16 -> 11;
            case UNORM_16 -> 13;
            case SNORM_16 -> 15;
            case FLOAT_16 -> 17;
            case UINT_32 -> 19;
            case SINT_32 -> 23;
            case FLOAT_32 -> 27;
            default -> throw new IllegalArgumentException("Unsupported vertex format " + format);
        };
        return start + (start >= 19 ? c - 1 : c / 2 - 1);
    }
}
