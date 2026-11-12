package dev.raynium.client.takeover;

import com.mojang.blaze3d.vertex.VertexConsumer;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.bridge.NativeFrameBuffer;
import net.minecraft.client.gui.navigation.ScreenRectangle;
import net.minecraft.client.gui.render.TextureSetup;
import net.minecraft.client.renderer.state.gui.GuiElementRenderState;
import net.minecraft.client.renderer.state.gui.GuiRenderState;

public final class TakeoverGui implements VertexConsumer {
    private static final org.slf4j.Logger LOGGER=org.slf4j.LoggerFactory.getLogger("raynium.takeover");
    private static final java.util.Set<net.minecraft.resources.Identifier> UNSUPPORTED=new java.util.HashSet<>();
    public interface Source { void raynium$capture(NativeFrameBuffer buffer); }
    private final NativeFrameBuffer buffer;
    private RenderPipeline pipeline;
    private TextureSetup texture;
    private ScreenRectangle scissor;
    private int first;
    private int width,height;
    private boolean pending;
    private float x,y,z,u,v;
    private int color=-1;
    public TakeoverGui(NativeFrameBuffer buffer) { this.buffer=buffer; }
    public void capture(GuiRenderState state) {
        var window=net.minecraft.client.Minecraft.getInstance().gameRenderer.gameRenderState().windowRenderState;
        width=(int)Math.ceil((double)window.width/window.guiScale); height=(int)Math.ceil((double)window.height/window.guiScale);
        buffer.beginGui(0,0);
        state.forEachElement(this::element,GuiRenderState.TraverseRange.BEFORE_BLUR);
        finishDraw(); pipeline=null;
        state.forEachElement(this::element,GuiRenderState.TraverseRange.AFTER_BLUR);
        finishDraw();
    }
    private void element(GuiElementRenderState element) {
        if(pipelineKind(element.pipeline())<0) {
            if(UNSUPPORTED.add(element.pipeline().getLocation())) LOGGER.warn("Skipping unsupported GUI pipeline {}",element.pipeline().getLocation());
            return;
        }
        if(pipeline!=element.pipeline() || !element.textureSetup().equals(texture) || !java.util.Objects.equals(scissor,element.scissorArea())) {
            finishDraw(); pipeline=element.pipeline(); texture=element.textureSetup(); scissor=element.scissorArea(); first=buffer.guiVertexCount();
        }
        element.buildVertices(this); flush();
    }
    private static int pipelineKind(RenderPipeline pipeline) {
        return pipeline.getLocation().getNamespace().equals("minecraft")?switch(pipeline.getLocation().getPath()) {
            case "pipeline/gui"->0; case "pipeline/gui_invert"->1; case "pipeline/gui_text_highlight"->2;
            case "pipeline/gui_textured"->3; case "pipeline/gui_textured_premultiplied_alpha"->4;
            case "pipeline/gui_opaque_textured_background"->5; case "pipeline/gui_nausea_overlay"->6;
            case "pipeline/vignette"->7; case "pipeline/crosshair"->8; case "pipeline/mojang_logo"->9;
            case "pipeline/gui_text"->10; case "pipeline/gui_text_grayscale"->11;
            default->-1;
        }:-1;
    }
    private void finishDraw() {
        flush();
        int count=buffer.guiVertexCount()-first;
        if(pipeline==null || count==0) return;
        int kind=pipelineKind(pipeline);
        int id=0,bits=0;
        if(kind>=3) {
            var view=texture.texure0();
            if(view==null || !(view.texture() instanceof TakeoverHandles.Texture image) || image.nativeTexture()==null)
                throw new IllegalStateException("takeover: Missing GUI texture for "+pipeline);
            id=image.nativeTexture().id();
            var sampler=texture.sampler0();
            if(sampler.getAddressModeU()==AddressMode.REPEAT) bits|=1;
            if(sampler.getAddressModeV()==AddressMode.REPEAT) bits|=2;
            if(sampler.getMinFilter()==FilterMode.LINEAR) bits|=4;
            if(sampler.getMagFilter()==FilterMode.LINEAR) bits|=8;
            if(sampler.getMaxLod().isEmpty()) bits|=16;
            else if(sampler.getMaxLod().getAsDouble()==0) bits|=32;
        }
        if(count%4!=0) throw new IllegalStateException("takeover: GUI requires complete quads");
        buffer.guiDraw(kind,id,bits,first,count,scissor==null?0:scissor.left(),scissor==null?0:scissor.top(),
            scissor==null?width:scissor.width(),scissor==null?height:scissor.height());
        first=buffer.guiVertexCount();
    }
    private void flush() { if(pending) { buffer.guiVertex(x,y,z,u,v,color); pending=false; } }
    @Override public VertexConsumer addVertex(float x,float y,float z) { flush(); this.x=x; this.y=y; this.z=z; u=v=0; color=-1; pending=true; return this; }
    @Override public VertexConsumer setColor(int r,int g,int b,int a) { color=r|(g<<8)|(b<<16)|(a<<24); return this; }
    @Override public VertexConsumer setColor(int argb) { return setColor((argb>>>16)&255,(argb>>>8)&255,argb&255,argb>>>24); }
    @Override public VertexConsumer setUv(float u,float v) { this.u=u; this.v=v; return this; }
    @Override public VertexConsumer setUv1(int u,int v) { return this; }
    @Override public VertexConsumer setUv2(int u,int v) { return this; }
    @Override public VertexConsumer setUv3(float u,float v) { return this; }
    @Override public VertexConsumer setNormal(float x,float y,float z) { return this; }
    @Override public VertexConsumer setLineWidth(float width) { return this; }
}
