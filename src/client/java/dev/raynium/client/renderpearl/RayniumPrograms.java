package dev.raynium.client.renderpearl;

import com.google.gson.Gson;
import com.google.gson.reflect.TypeToken;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.pipeline.*;
import dev.raynium.bridge.NativeTexture;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.*;
import java.util.regex.*;
import net.minecraft.resources.Identifier;
import org.lwjgl.util.shaderc.ShadercIncludeResult;

final class RayniumPrograms {
    record Input(int location,String type) {}
    record Resource(int group,int binding,String type,int sampler,String texelFormat) {}
    record Program(String program,Map<String,Input> inputs,Map<String,Resource> resources,int pushBytes) {}
    record Route(int group,int binding,int sampler,UniformType type,GpuFormat format,boolean cube) {}
    record Preparation(String name,int[] words,List<Route> routes,List<String> uniforms,byte[] pack) {}
    private static final Pattern INCLUDE=Pattern.compile("(?m)^\\s*#include\\s+<([^>]+)>\\s*$");
    private final Map<String,Program> programs;

    private final Map<String,String> terrainKeys;
    private final AtomicBoolean warnedTerrain=new AtomicBoolean();
    private final ShaderProgramCache cache;

    RayniumPrograms(Path gameDirectory) {
        cache=new ShaderProgramCache(gameDirectory);
        try(var stream=Objects.requireNonNull(RayniumPrograms.class.getResourceAsStream("/assets/raynium/shaderpacks/terrain-programs.json"));
            var reader=new InputStreamReader(stream,StandardCharsets.UTF_8)) {
            terrainKeys=Map.copyOf(new Gson().fromJson(reader,new TypeToken<Map<String,String>>(){}));
        } catch(IOException e) { throw new UncheckedIOException(e); }
        var stream=RayniumPrograms.class.getResourceAsStream("/assets/raynium/shaderpacks/default.json");
        if(stream==null) throw new IllegalStateException("Missing RenderPearl program catalog");
        try(var reader=new InputStreamReader(stream,StandardCharsets.UTF_8)) {
            programs=Map.copyOf(new Gson().fromJson(reader,new TypeToken<Map<String,Program>>(){}));
        } catch(IOException e) { throw new UncheckedIOException(e); }
    }

    static String expand(String source,ShaderSource shaders,Set<Identifier> stack) {
        if(source==null) throw new IllegalArgumentException("Missing shader source");
        return INCLUDE.matcher(source.replace("\r\n","\n").replace('\r','\n')).replaceAll(match->{
            var id=Identifier.parse(match.group(1));
            if(!stack.add(id)) throw new IllegalArgumentException("Recursive shader include "+id);
            try {
                var entry=shaders.getInclude(id);
                if(entry==null) throw new IllegalArgumentException("Missing shader include "+id);
                var include=ShadercIncludeResult.create(entry.includeResultPtr());
                long address=org.lwjgl.system.MemoryUtil.memGetAddress(include.address()+ShadercIncludeResult.CONTENT);
                var bytes=org.lwjgl.system.MemoryUtil.memByteBuffer(address,Math.toIntExact(include.content_length()));
                return Matcher.quoteReplacement(expand(StandardCharsets.UTF_8.decode(bytes).toString(),shaders,stack));
            } finally { stack.remove(id); }
        });
    }

    private static String digest(String input) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(input.getBytes(StandardCharsets.UTF_8)));
        } catch(NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }

    private static List<String> defineFields(net.minecraft.client.renderer.ShaderDefines defines) {
        var fields=new ArrayList<String>();
        var sorted=new TreeMap<String,String>();
        defines.flags().forEach(flag->sorted.put(flag,""));
        sorted.putAll(defines.values());
        sorted.forEach((name,value)->{ fields.add(name); fields.add(value); });
        return fields;
    }

    static String key(String vertex,String fragment,net.minecraft.client.renderer.ShaderDefines defines) {
        var fields=new ArrayList<>(List.of(digest(vertex),digest(fragment)));
        fields.addAll(defineFields(defines));
        return "p_"+digest(String.join("\0",fields));
    }

    Preparation prepare(RenderPipeline pipeline,ShaderSource source) {
        var stages=pipeline.getShaders();
        var defines=pipeline.getShaderDefines();
        var terrain=Identifier.parse("minecraft:core/terrain");
        String fallback=terrain.equals(stages.get(ShaderType.VERTEX)) && terrain.equals(stages.get(ShaderType.FRAGMENT))
            ?terrainKeys.get(digest(String.join("\0",defineFields(defines)))):null;
        String vertex,fragment;
        try {
            vertex=expand(source.getShader(stages.get(ShaderType.VERTEX),ShaderType.VERTEX),source,new HashSet<>());
            fragment=expand(source.getShader(stages.get(ShaderType.FRAGMENT),ShaderType.FRAGMENT),source,new HashSet<>());
        } catch(IllegalArgumentException error) {
            if(fallback!=null) return retained(pipeline,fallback);
            throw error;
        }
        String key=key(vertex,fragment,defines);
        if(fallback!=null && !fallback.equals(key)) return retained(pipeline,fallback);
        var builtin=programs.get(key);
        if(builtin!=null) return prepare(pipeline,builtin,null);
        var external=cache.resolve(key,vertex,fragment,pipeline);
        return prepare(pipeline,external.program(),external.pack());
    }

    private Preparation retained(RenderPipeline pipeline,String key) {
        if(warnedTerrain.compareAndSet(false,true))
            org.slf4j.LoggerFactory.getLogger("raynium").warn("Retained terrain uses the built-in shader; the resource pack terrain override is ignored");
        return prepare(pipeline,Objects.requireNonNull(programs.get(key)),null);
    }

    static Preparation prepare(RenderPipeline pipeline,Program program,byte[] pack) {
        if(program.pushBytes()>pipeline.pushConstantSize()) throw new IllegalArgumentException("Push constant range exceeds pipeline layout");
        var declared=BindGroupLayout.flattenUniforms(pipeline.getBindGroupLayouts());
        var routes=new ArrayList<Route>();
        var names=new ArrayList<String>();
        for(var entry:new TreeMap<>(program.resources()).entrySet()) {
            var r=entry.getValue();
            var uniform=declared.stream().filter(u->u.name().equals(entry.getKey())).findFirst().orElseThrow(
                ()->new IllegalArgumentException("Missing shader resource "+entry.getKey()));
            var type=switch(r.type().replaceFirst("^[iu]sampler","sampler")) {
                case "ubo" -> UniformType.UNIFORM_BUFFER;
                case "sampler2D","samplerCube","sampler2DRect" -> UniformType.COMBINED_IMAGE_SAMPLER;
                case "samplerBuffer" -> UniformType.TEXEL_BUFFER;
                default -> throw new IllegalArgumentException("Unsupported shader resource type "+r.type());
            };
            if(uniform.type()!=type) throw new IllegalArgumentException("Resource type mismatch for "+entry.getKey());
            if(type==UniformType.TEXEL_BUFFER) {
                NativeTexture.Format.from(Objects.requireNonNull(uniform.gpuFormat()));
                if(!uniform.gpuFormat().name().equals(r.texelFormat())) throw new IllegalArgumentException("Texel buffer format mismatch for "+uniform.name());
            }
            names.add(uniform.name());
            routes.add(new Route(r.group(),r.binding(),type==UniformType.COMBINED_IMAGE_SAMPLER?r.sampler():-1,type,uniform.gpuFormat(),r.type().endsWith("Cube")));
        }
        var bindings=new ArrayList<Integer>(); var attributes=new ArrayList<Integer>();
        var used=new HashSet<String>();
        var formats=pipeline.getVertexFormatBindings();
        for(int slot=0;slot<formats.size();++slot) {
            var format=formats.get(slot);
            if(format==null) continue;
            Collections.addAll(bindings,slot,format.getVertexSize(),
                pipeline.getPrimitiveTopology()==PrimitiveTopology.POINTS && slot==0?1:format.getStepRate());
            for(var element:format.getElements()) {
                var input=program.inputs().get(element.name());
                if(input==null) continue;
                if(!used.add(element.name())) throw new IllegalArgumentException("Duplicate active vertex element "+element.name());
                int value=vertexFormat(element.format(),input.type());
                Collections.addAll(attributes,slot,input.location(),element.offset(),value);
            }
        }
        if(used.size()!=program.inputs().size()) throw new IllegalArgumentException("Missing active vertex input");
        var words=new ArrayList<Integer>();
        int topology=switch(pipeline.getPrimitiveTopology()) {
            case DEBUG_LINES -> 1; case DEBUG_LINE_STRIP -> 2; case TRIANGLE_STRIP -> 3; default -> 0;
        };
        var depth=pipeline.getDepthStencilState();
        int compare=depth==null?8:switch(depth.depthTest()) {
            case NEVER_PASS->0; case LESS_THAN->1; case EQUAL->2; case LESS_THAN_OR_EQUAL->3;
            case GREATER_THAN->4; case NOT_EQUAL->5; case GREATER_THAN_OR_EQUAL->6; case ALWAYS_PASS->7;
        };
        var colors=pipeline.getColorTargetStates();
        Collections.addAll(words,topology,(pipeline.isCull()?1:0)|(pipeline.getPolygonMode()==PolygonMode.WIREFRAME?2:0),
            compare,depth!=null && depth.writeDepth()?1:0,depth==null?0:Float.floatToIntBits(depth.depthBiasConstant()),
            depth==null?0:Float.floatToIntBits(depth.depthBiasScaleFactor()),bindings.size()/3,attributes.size()/4,colors.size());
        words.addAll(bindings); words.addAll(attributes);
        for(var color:colors) {
            if(color==null) throw new IllegalArgumentException("Unbound color target slot");
            var blend=color.blendFunction().orElse(null);
            Collections.addAll(words,NativeTexture.Format.from(color.format()).ordinal(),color.writeMask(),blend==null?0:1);
            if(blend==null) words.addAll(Collections.nCopies(6,0));
            else for(var equation:List.of(blend.color(),blend.alpha()))
                Collections.addAll(words,blendFactor(equation.sourceFactor()),blendFactor(equation.destFactor()),equation.op().ordinal());
        }
        return new Preparation(program.program(),words.stream().mapToInt(i->i).toArray(),List.copyOf(routes),List.copyOf(names),pack);
    }

    private static int blendFactor(BlendFactor f) {
        return switch(f) {
            case ZERO->0; case ONE->1; case SRC_COLOR->2; case ONE_MINUS_SRC_COLOR->3; case SRC_ALPHA->4;
            case ONE_MINUS_SRC_ALPHA->5; case DST_COLOR->6; case ONE_MINUS_DST_COLOR->7; case DST_ALPHA->8;
            case ONE_MINUS_DST_ALPHA->9; case SRC_ALPHA_SATURATE->10; case CONSTANT_ALPHA,CONSTANT_COLOR->11;
            case ONE_MINUS_CONSTANT_ALPHA,ONE_MINUS_CONSTANT_COLOR->12;
        };
    }

    private static int vertexFormat(GpuFormat format,String type) {
        int c=format.componentCount();
        String base=switch(format.componentType()) {
            case UINT_8,UINT_16,UINT_32->"u"; case SINT_8,SINT_16,SINT_32->"i"; default->"";
        };
        boolean compatible=false;
        for(int n=1;n<=c;++n)
            compatible|=type.equals(n==1?(base.isEmpty()?"float":base.equals("i")?"int":"uint"):base+"vec"+n);
        if(!compatible || (format.blockSize()/c!=4 && c!=2 && c!=4)) throw new IllegalArgumentException("Vertex "+format+" cannot supply "+type);
        int start=switch(format.componentType()) {
            case UINT_8->1; case SINT_8->3; case UNORM_8->5; case SNORM_8->7; case UINT_16->9; case SINT_16->11;
            case UNORM_16->13; case SNORM_16->15; case FLOAT_16->17; case UINT_32->19; case SINT_32->23; case FLOAT_32->27;
            default->throw new IllegalArgumentException("Unsupported vertex format "+format);
        };
        return start+(start>=19?c-1:c/2-1);
    }
}
