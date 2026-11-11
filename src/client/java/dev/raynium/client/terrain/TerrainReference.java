package dev.raynium.client.terrain;

import com.mojang.blaze3d.pipeline.TextureTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.commands.*;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.client.preview.PreviewOverlay;
import java.nio.file.*;
import java.util.*;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.chunk.*;
import net.minecraft.client.renderer.oit.*;
import org.joml.Vector4f;
import org.slf4j.LoggerFactory;

/** Opt-in, same-frame MC terrain reference. No work or readback when compareDir is unset. */
public final class TerrainReference {
    private static final String DIRECTORY=System.getProperty("raynium.terrain.compareDir","");
    private static final int INTERVAL=Math.max(1,Integer.getInteger("raynium.terrain.compareInterval",120));
    private static final java.util.List<TextureTarget> targets=new ArrayList<>();
    private static ChunkSectionsToRender draws;
    private static TextureTarget reference;
    private static GpuQueryPool query;
    private static long frame;
    private static boolean rendered;
    private static int width,height;
    private TerrainReference() {}
    public static void beginFrame() { if(query!=null) { query.close(); query=null; } frame++; draws=null; rendered=false; }
    public static void prepared(ChunkSectionsToRender value) { if(!DIRECTORY.isEmpty() && frame%INTERVAL==0) draws=value; }
    private static TextureTarget target(String name,GpuFormat color,GpuFormat depth) {
        var target=new TextureTarget("Raynium reference "+name,width,height,color,depth); targets.add(target); return target;
    }
    public static void close() {
        if(query!=null) { query.close(); query=null; }
        for(var target:targets) target.destroyBuffers(); targets.clear(); reference=null; draws=null;
    }
    public static void render(Minecraft client,GpuSampler sampler) {
        if(draws==null || TerrainCapture.snapshot()==null) return;
        var region=PreviewOverlay.Region.of(client.gameRenderer.mainRenderTarget().width,client.gameRenderer.mainRenderTarget().height);
        if(reference==null || width!=region.width() || height!=region.height()) {
            var current=draws; close(); draws=current; width=region.width(); height=region.height();
            reference=target("color",GpuFormat.RGBA8_UNORM,GpuFormat.D32_FLOAT);
            target("bounds",GpuFormat.RGBA32_FLOAT,null); target("culled",GpuFormat.RGBA32_FLOAT,null);
            target("coeff0",GpuFormat.RGBA16_FLOAT,null); target("coeff1",GpuFormat.RGBA16_FLOAT,null);
            target("accumulate",GpuFormat.RGBA16_FLOAT,null);
        }
        // Resize discards target owners but not this frame's draw list.
        var snapshot=TerrainCapture.snapshot();
        var encoder=RenderSystem.getDevice().createCommandEncoder();
        query=RenderSystem.getDevice().createTimestampQueryPool(2); encoder.writeTimestamp(query,0);
        var atlas=TerrainTextures.atlas().getTextureView();
        try(var pass=encoder.createRenderPass(()->"Raynium MC terrain reference",reference.getColorTextureView(),
            Optional.of(new Vector4f(snapshot.fog[0],snapshot.fog[1],snapshot.fog[2],0)),reference.getDepthTextureView(),OptionalDouble.of(0))) {
            RenderSystem.bindDefaultUniforms(pass);
            draws.renderGroup(ChunkSectionLayerGroup.OPAQUE,pass,sampler,atlas,false);
            if(!snapshot.improvedTransparency) draws.renderGroup(ChunkSectionLayerGroup.TRANSLUCENT,pass,sampler,atlas,false);
        }
        if(snapshot.improvedTransparency) {
            var bounds=targets.get(1); var culled=targets.get(2);
            var coefficients=new GpuTextureView[]{targets.get(3).getColorTextureView(),targets.get(4).getColorTextureView()};
            encoder.clearColorTexture(bounds.getColorTexture(),new Vector4f(-Float.MAX_VALUE,0,0,0));
            for(int i=3;i<6;i++) encoder.clearColorTexture(targets.get(i).getColorTexture(),new Vector4f(0));
            var params=new OitRenderPassProvider.Parameters(bounds.getColorTextureView(),coefficients,
                targets.get(5).getColorTextureView(),reference.getDepthTextureView());
            for(var stage:OitStage.values()) {
                draws.renderOit(sampler,stage,params,atlas,client.gameRenderer.lightmap());
                if(stage==OitStage.DEPTH_BOUNDS) {

                    try(var pass=encoder.createRenderPass(()->"Raynium reference bounds cull",culled.getColorTextureView(),
                        Optional.empty(),reference.getDepthTextureView(),OptionalDouble.empty())) {
                        RenderSystem.bindDefaultUniforms(pass);
                        pass.setPipeline(RenderSystem.getCompiledPipeline(RenderPipelines.BLIT_DEPTH_BOUNDS));
                        pass.setUniform("InSampler",bounds.getColorTextureView(),RenderSystem.getSamplerCache().getClampToEdge(FilterMode.NEAREST));
                        pass.draw(3,1,0,0);
                        pass.setPipeline(RenderSystem.getCompiledPipeline(RenderPipelines.OIT_DEPTH_BOUNDS_CULL));
                        pass.setUniform("DepthBoundsSampler",bounds.getColorTextureView(),RenderSystem.getSamplerCache().getClampToEdge(FilterMode.NEAREST));
                        pass.draw(3,1,0,0);
                    }
                    params.setDepthBoundsTargetView(culled.getColorTextureView());
                }
            }
            try(var pass=encoder.createRenderPass(()->"Raynium reference composite",reference.getColorTextureView(),Optional.empty(),
                reference.getDepthTextureView(),OptionalDouble.empty())) {
                RenderSystem.bindDefaultUniforms(pass); pass.setPipeline(RenderSystem.getCompiledPipeline(RenderPipelines.OIT_COMPOSITE));
                var nearest=RenderSystem.getSamplerCache().getClampToEdge(FilterMode.NEAREST);
                pass.setUniform("Sampler0",targets.get(5).getColorTextureView(),nearest);
                for(int i=0;i<2;i++) pass.setUniform("Coeff"+i,coefficients[i],nearest);
                pass.setUniform("DepthBoundsSampler",bounds.getColorTextureView(),nearest); pass.draw(3,1,0,0);
            }
        }
        encoder.writeTimestamp(query,1);
        rendered=true; draws=null;
    }
    public static void capture(Minecraft client,PreviewOverlay.Region region,boolean stable) {
        if(!rendered || !stable) return;
        rendered=false;
        final GpuQueryPool timings=query; query=null;
        final double period=RenderSystem.getDevice().getDeviceInfo().timestampPeriod();
        final double[] gpuMs={Double.NaN};
        final long sequence=frame;
        final boolean oit=TerrainCapture.snapshot().improvedTransparency;
        final int w=width,h=height;
        final Path directory=Path.of(DIRECTORY);
        final java.awt.image.BufferedImage[] pair=new java.awt.image.BufferedImage[2];
        java.util.function.BiConsumer<Integer,java.awt.image.BufferedImage> accept=(index,image)->{
            pair[index]=image;
            if(pair[0]==null || pair[1]==null) return;
            var diff=new java.awt.image.BufferedImage(w,h,java.awt.image.BufferedImage.TYPE_INT_RGB);
            long total=0; int maximum=0;
            for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
                int a=pair[0].getRGB(x,y),b=pair[1].getRGB(x,y),rgb=0;
                for(int shift=0;shift<=16;shift+=8) { int delta=Math.abs(((a>>>shift)&255)-((b>>>shift)&255)); total+=delta; maximum=Math.max(maximum,delta); rgb|=delta<<shift; }
                diff.setRGB(x,y,rgb);
            }
            try {
                Files.createDirectories(directory); String prefix=String.format("%08d-%s",sequence,oit?"oit":"classic");
                javax.imageio.ImageIO.write(pair[0],"png",directory.resolve(prefix+"-mc.png").toFile());
                javax.imageio.ImageIO.write(pair[1],"png",directory.resolve(prefix+"-native.png").toFile());
                javax.imageio.ImageIO.write(diff,"png",directory.resolve(prefix+"-diff.png").toFile());
                Files.writeString(directory.resolve(prefix+".json"),"{\"width\":"+w+",\"height\":"+h+",\"meanAbsoluteRgbError\":"+(total/(double)(w*h*3))+",\"maximumRgbError\":"+maximum+",\"mcTerrainGpuMs\":"+(Double.isFinite(gpuMs[0])?Double.toString(gpuMs[0]):"null")+"}");
            } catch(java.io.IOException failure) { LoggerFactory.getLogger("raynium.compare").error("Cannot save terrain comparison",failure); }
        };
        Screenshot.takeScreenshot(reference,image->{
            try(image;timings) {
                var ticks=timings.getValues(0,2);
                if(ticks[0].isPresent() && ticks[1].isPresent()) gpuMs[0]=(ticks[1].getAsLong()-ticks[0].getAsLong())*period/1e6;
                var result=new java.awt.image.BufferedImage(w,h,java.awt.image.BufferedImage.TYPE_INT_RGB);
                for(int y=0;y<h;y++) for(int x=0;x<w;x++) result.setRGB(x,y,image.getPixel(x,y)); accept.accept(0,result);
            }
        });
        int y0=client.gameRenderer.mainRenderTarget().height-region.y()-h;
        Screenshot.takeScreenshot(client.gameRenderer.mainRenderTarget(),image->{
            try(image) { var result=new java.awt.image.BufferedImage(w,h,java.awt.image.BufferedImage.TYPE_INT_RGB);
                for(int y=0;y<h;y++) for(int x=0;x<w;x++) result.setRGB(x,y,image.getPixel(region.x()+x,y0+y)); accept.accept(1,result); }
        });
    }
}
