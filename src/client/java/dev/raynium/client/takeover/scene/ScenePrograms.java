package dev.raynium.client.takeover.scene;

import com.google.gson.Gson;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.takeover.TakeoverHandles;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Offline interface slots are tied to the full vanilla pipeline identity, including macro variants. */
public final class ScenePrograms {
    public record Constant(int binding,int size) {}
    public record Texture(int texture,int sampler) {}
    public record Program(int id,List<String> names,String program,Map<String,Constant> constants,Map<String,Texture> textures,Map<String,Integer> texels) {
        public void constant(NativeSceneFrame frame,String name,int blob) {
            var slot=constants.get(name); if(slot!=null) frame.constant(slot.binding(),blob,0,256);
        }
        public void texture(NativeSceneFrame frame,String name,GpuTextureView view,GpuSampler sampler) {
            var slot=textures.get(name); if(slot==null) return;
            if(view.isClosed() || sampler.isClosed() || !(view.texture() instanceof TakeoverHandles.Texture texture))
                throw new IllegalArgumentException("Scene sampling view is not mapped: "+name);
            frame.texture(slot.texture(),texture.nativeTexture().id(),samplerBits(sampler),view.baseMipLevel(),view.mipLevels(),
                (float)sampler.getMaxLod().orElse(1000),sampler.getMaxAnisotropy());
        }
        public void oitTextures(NativeSceneFrame frame) {
            for(var entry:textures.entrySet()) {
                int resource=switch(entry.getKey()) { case "DepthBoundsSampler"->1; case "Coeff0"->2; case "Coeff1"->3; default->0; };
                if(resource!=0) frame.graphTexture(entry.getValue().texture(),resource);
            }
        }
    }
    private static final VanillaPipelineCatalog CATALOG=VanillaPipelineCatalog.create();
    private static final Map<Integer,Program> PROGRAMS=load();
    private ScenePrograms() {}
    private static Map<Integer,Program> load() {
        try(var stream=Objects.requireNonNull(ScenePrograms.class.getResourceAsStream("/assets/raynium/scene-interfaces.json"));
            var reader=new InputStreamReader(stream,StandardCharsets.UTF_8)) {
            var result=new HashMap<Integer,Program>();
            for(var program:new Gson().fromJson(reader,Program[].class)) {
                var entry=CATALOG.entries().get(program.id()-1);
                if(entry.id()!=program.id() || !entry.names().equals(program.names()) || program.program()==null || result.put(program.id(),program)!=null)
                    throw new IllegalStateException("Vanilla shader interface catalog differs from this Minecraft version");
            }
            if(result.size()!=CATALOG.entries().size()) throw new IllegalStateException("Incomplete vanilla shader interface catalog");
            return Map.copyOf(result);
        } catch(java.io.IOException failure) { throw new IllegalStateException("Cannot read vanilla shader interfaces",failure); }
    }
    public static Program require(RenderPipeline pipeline) { return PROGRAMS.get(CATALOG.require(pipeline).id()); }
    public static int samplerBits(GpuSampler sampler) {
        int bits=(sampler.getAddressModeU()==AddressMode.REPEAT?1:0)|(sampler.getAddressModeV()==AddressMode.REPEAT?2:0)
            |(sampler.getMinFilter()==FilterMode.LINEAR?4:0)|(sampler.getMagFilter()==FilterMode.LINEAR?8:0);
        if(sampler.getMaxLod().orElse(1000)>0.25) bits|=16;
        return bits;
    }
}
