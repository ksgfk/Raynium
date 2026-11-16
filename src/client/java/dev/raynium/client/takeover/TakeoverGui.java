package dev.raynium.client.takeover;

import com.mojang.blaze3d.vertex.VertexConsumer;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.textures.*;
import dev.raynium.bridge.NativeFrameBuffer;
import net.minecraft.client.gui.navigation.ScreenRectangle;
import net.minecraft.client.gui.render.TextureSetup;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.state.gui.GuiElementRenderState;
import net.minecraft.client.renderer.state.gui.GuiRenderState;

public final class TakeoverGui implements VertexConsumer,AutoCloseable {
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
    private final java.util.ArrayList<dev.raynium.client.takeover.scene.SceneMesh> sceneMeshes=new java.util.ArrayList<>();
    private dev.raynium.client.takeover.scene.SceneUniforms sceneUniforms;
    private int sceneTransforms;
    public TakeoverGui(NativeFrameBuffer buffer) { this.buffer=buffer; }
    public void capture(GuiRenderState state) {
        var window=net.minecraft.client.Minecraft.getInstance().gameRenderer.gameRenderState().windowRenderState;
        width=(int)Math.ceil((double)window.width/window.guiScale); height=(int)Math.ceil((double)window.height/window.guiScale);
        pipeline=null; pending=false; first=0; sceneUniforms=null;
        buffer.beginGui(0,0);
        state.forEachElement(this::element,GuiRenderState.TraverseRange.BEFORE_BLUR);
        finishDraw(); pipeline=null;
        int blurSplit=buffer.guiDrawCount();
        state.forEachElement(this::element,GuiRenderState.TraverseRange.AFTER_BLUR);
        finishDraw();
        if(buffer.guiDrawCount()>blurSplit) dev.raynium.client.takeover.scene.ScenePost.gui(buffer.scene(),
            net.minecraft.client.Minecraft.getInstance().gameRenderer.gameRenderState(),blurSplit);
    }
    private void element(GuiElementRenderState element) {
        if(pipelineKind(element.pipeline())==12) {
            finishDraw(); pipeline=null; sceneElement(element); first=buffer.guiVertexCount(); return;
        }
        if(pipeline!=element.pipeline() || !element.textureSetup().equals(texture) || !java.util.Objects.equals(scissor,element.scissorArea())) {
            finishDraw(); pipeline=element.pipeline(); texture=element.textureSetup(); scissor=element.scissorArea(); first=buffer.guiVertexCount();
        }
        element.buildVertices(this); flush();
    }
    static int pipelineKind(RenderPipeline pipeline) {
        if(pipeline==RenderPipelines.GUI) return 0;
        if(pipeline==RenderPipelines.GUI_INVERT) return 1;
        if(pipeline==RenderPipelines.GUI_TEXT_HIGHLIGHT) return 2;
        if(pipeline==RenderPipelines.GUI_TEXTURED) return 3;
        if(pipeline==RenderPipelines.GUI_TEXTURED_PREMULTIPLIED_ALPHA) return 4;
        if(pipeline==RenderPipelines.GUI_OPAQUE_TEXTURED_BACKGROUND) return 5;
        if(pipeline==RenderPipelines.GUI_NAUSEA_OVERLAY) return 6;
        if(pipeline==RenderPipelines.VIGNETTE) return 7;
        if(pipeline==RenderPipelines.CROSSHAIR) return 8;
        if(pipeline==RenderPipelines.MOJANG_LOGO) return 9;
        if(pipeline==RenderPipelines.GUI_TEXT) return 10;
        if(pipeline==RenderPipelines.GUI_TEXT_GRAYSCALE) return 11;
        if(pipeline==RenderPipelines.END_PORTAL || pipeline==RenderPipelines.END_GATEWAY) return 12;
        throw new IllegalArgumentException("Unmapped GUI pipeline variant: "+pipeline.getLocation()+"; custom drawing pipelines are unsupported");
    }
    private void sceneElement(GuiElementRenderState element) {
        var scene=buffer.scene(); var state=net.minecraft.client.Minecraft.getInstance().gameRenderer.gameRenderState(); var window=state.windowRenderState;
        var program=dev.raynium.client.takeover.scene.ScenePrograms.require(element.pipeline());
        if(sceneUniforms==null) {
            var projection=new org.joml.Matrix4f().setOrtho(0,(float)window.width/window.guiScale,(float)window.height/window.guiScale,0,11000,1000,true);
            var view=new org.joml.Matrix4f().translation(0,0,-11000);
            sceneUniforms=new dev.raynium.client.takeover.scene.SceneUniforms(scene,state,projection,view,net.minecraft.world.level.CardinalLighting.Type.DEFAULT);
            sceneTransforms=sceneUniforms.transforms(view,new org.joml.Matrix4f());
        }
        dev.raynium.client.takeover.scene.SceneMesh mesh=null;
        for(var candidate:sceneMeshes) if(!candidate.borrowed()) { mesh=candidate; break; }
        if(mesh==null) { mesh=new dev.raynium.client.takeover.scene.SceneMesh(); sceneMeshes.add(mesh); }
        TakeoverRuntime.retain(mesh);
        var builder=mesh.begin(element.pipeline().getPrimitiveTopology(),element.pipeline().getVertexFormatBinding(0));
        element.buildVertices(builder); int vertices=mesh.finish(builder,scene),first=scene.bindingCount();
        sceneUniforms.bind(program,sceneTransforms); var textures=element.textureSetup();
        if(textures.texure0()!=null) program.texture(scene,"Sampler0",textures.texure0(),textures.sampler0());
        if(textures.texure1()!=null) program.texture(scene,"Sampler1",textures.texure1(),textures.sampler1());
        if(textures.texure2()!=null) program.texture(scene,"Sampler2",textures.texure2(),textures.sampler2());
        var area=element.scissorArea();
        int left=area==null?0:Math.clamp(area.left()*window.guiScale,0,window.width),top=area==null?0:Math.clamp(area.top()*window.guiScale,0,window.height);
        int right=area==null?window.width:Math.clamp(area.right()*window.guiScale,left,window.width),bottom=area==null?window.height:Math.clamp(area.bottom()*window.guiScale,top,window.height);
        int draw=scene.draw(program.id(),dev.raynium.bridge.NativeSceneFrame.GUI,vertices,-1,mesh.vertexCount(),0,0,first,1,-1,0,0,0,left,top,right-left,bottom-top);
        buffer.guiDraw(12,draw,0,buffer.guiVertexCount(),0,0,0,width,height);
    }
    @Override public void close() { for(var mesh:sceneMeshes) mesh.close(); sceneMeshes.clear(); }
    private void finishDraw() {
        flush();
        int count=buffer.guiVertexCount()-first;
        if(pipeline==null || count==0) return;
        int kind=pipelineKind(pipeline);
        int id=0,bits=0,baseMip=0,mipCount=0,anisotropy=1; float maxLod=1000;
        if(kind>=3) {
            var view=texture.texure0();
            if(view==null || !(view.texture() instanceof TakeoverHandles.Texture image) || image.nativeTexture()==null)
                throw new IllegalStateException("takeover: Missing GUI texture for "+pipeline);
            id=image.nativeTexture().id();
            var sampler=texture.sampler0();
            if(view.isClosed() || sampler.isClosed()) throw new IllegalStateException("Closed GUI texture sampling");
            bits=dev.raynium.client.takeover.scene.ScenePrograms.samplerBits(sampler);
            baseMip=view.baseMipLevel(); mipCount=view.mipLevels(); maxLod=(float)sampler.getMaxLod().orElse(1000); anisotropy=sampler.getMaxAnisotropy();
        }
        if(count%4!=0) throw new IllegalStateException("takeover: GUI requires complete quads");
        buffer.guiDraw(kind,id,bits,first,count,scissor==null?0:scissor.left(),scissor==null?0:scissor.top(),
            scissor==null?width:scissor.width(),scissor==null?height:scissor.height(),baseMip,mipCount,maxLod,anisotropy);
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
