package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.commands.RenderPassDescriptor;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.bridge.NativeTexture;
import java.util.*;
import org.joml.Vector4f;

public final class TerrainTestPass {
    private TerrainTestPass() {}
    public static long submitMeasured(CommandTape tape) { return tape.submitSequence(true); }
    public static long queryPoolHandle(RayniumQueryPool pool) { return pool.handle(); }
    public static CommandTape batch(RayniumGpuDevice device) { return device.currentBatch(); }
    public static void loadAttachments(CommandTape tape,NativeTexture color,NativeTexture depth) {
        try(var target=new RayniumTextureView(color,0,1); var z=new RayniumTextureView(depth,0,1)) {
            var pass=RenderPassDescriptor.builder(()->"load cleared attachments").withColorAttachment(target,Optional.empty())
                .withDepthAttachment(z,OptionalDouble.empty()).build();
            tape.beginRaster(pass);
            tape.endRaster();
        }
    }
    public static void record(CommandTape tape,CommandTape.TerrainView view,NativeTexture color,NativeTexture depth,
                              NativeTexture atlas,NativeTexture light) {
        try(var target=new RayniumTextureView(color,0,1); var z=new RayniumTextureView(depth,0,1);
            var block=new RayniumTextureView(atlas,0,atlas.mipLevels()); var lighting=new RayniumTextureView(light,0,light.mipLevels());
            var sampler=new RayniumSampler(AddressMode.CLAMP_TO_EDGE,AddressMode.CLAMP_TO_EDGE,FilterMode.LINEAR,FilterMode.LINEAR,1,OptionalDouble.empty())) {
            var pass=RenderPassDescriptor.builder(()->"retained view").withColorAttachment(target,Optional.of(new Vector4f(0,0,0,1)))
                .withDepthAttachment(z,OptionalDouble.of(0)).build();
            tape.beginRaster(pass);
            var area=pass.renderArea();
            for(int layer=0;layer<3;++layer) tape.terrain(view,layer,area.x(),area.y(),area.width(),area.height(),new TextureViewAndSampler(block,sampler),
                new TextureViewAndSampler(lighting,sampler),new GpuTextureView[0]);
            tape.endRaster();
        }
    }
    public static void recordOit(CommandTape tape,CommandTape.TerrainView view,NativeTexture depth,NativeTexture atlas,NativeTexture light,
                                 NativeTexture bounds,NativeTexture first,NativeTexture second,NativeTexture accumulation) {
        try(var z=new RayniumTextureView(depth,0,1); var block=new RayniumTextureView(atlas,0,1); var lighting=new RayniumTextureView(light,0,1);
            var b=new RayniumTextureView(bounds,0,1); var c0=new RayniumTextureView(first,0,1); var c1=new RayniumTextureView(second,0,1);
            var acc=new RayniumTextureView(accumulation,0,1);
            var sampler=new RayniumSampler(AddressMode.CLAMP_TO_EDGE,AddressMode.CLAMP_TO_EDGE,FilterMode.NEAREST,FilterMode.NEAREST,1,OptionalDouble.empty())) {
            var atlasBinding=new TextureViewAndSampler(block,sampler);
            var lightBinding=new TextureViewAndSampler(lighting,sampler);
            var targets=new GpuTextureView[][]{{b},{c0,c1},{acc}};
            var sources=new GpuTextureView[][]{{},{b},{b,c0,c1}};
            for(int i=0;i<3;i++) {
                var pass=RenderPassDescriptor.builder(()->"terrain fade OIT").withDepthAttachment(z,OptionalDouble.of(0));
                for(var target:targets[i]) pass.withColorAttachment(target,Optional.of(new Vector4f()));
                tape.beginRaster(pass.build());
                tape.terrain(view,3+i,0,0,depth.getWidth(0),depth.getHeight(0),atlasBinding,lightBinding,sources[i]);
                tape.endRaster();
            }
        }
    }
}
