package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.commands.RenderPassDescriptor;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.bridge.NativeTexture;
import java.util.*;
import org.joml.Vector4f;

public final class TerrainTestPass {
    private TerrainTestPass() {}
    public static long submitMeasured(CommandTape tape) { return tape.submit(true).sequence(); }
    public static int queryPoolId(RayniumQueryPool pool) { return pool.id(); }
    public static CommandTape batch(RayniumGpuDevice device) { return device.currentBatch(); }
    public static void record(CommandTape tape,CommandTape.TerrainView view,NativeTexture color,NativeTexture depth,
                              NativeTexture atlas,NativeTexture light) {
        try(var target=new RayniumTextureView(color,0,1); var z=new RayniumTextureView(depth,0,1);
            var block=new RayniumTextureView(atlas,0,atlas.mipLevels()); var lighting=new RayniumTextureView(light,0,light.mipLevels());
            var sampler=new RayniumSampler(AddressMode.CLAMP_TO_EDGE,AddressMode.CLAMP_TO_EDGE,FilterMode.LINEAR,FilterMode.LINEAR,1,OptionalDouble.empty())) {
            var pass=RenderPassDescriptor.builder(()->"retained view").withColorAttachment(target,Optional.of(new Vector4f(0,0,0,1)))
                .withDepthAttachment(z,OptionalDouble.of(0)).build();
            tape.beginRaster(pass);
            for(int layer=0;layer<3;++layer) tape.terrain(view,layer,pass.renderArea(),new TextureViewAndSampler(block,sampler),
                new TextureViewAndSampler(lighting,sampler),new GpuTextureView[0]);
            tape.endRaster();
        }
    }
}
