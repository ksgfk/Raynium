package dev.raynium.client.mixin.takeover;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.*;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import com.mojang.renderpearl.api.textures.FilterMode;
import dev.raynium.bridge.NativeSceneFrame;
import dev.raynium.client.takeover.TakeoverRuntime;
import dev.raynium.client.takeover.scene.*;
import java.util.*;
import net.minecraft.client.renderer.*;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.texture.TextureManager;
import net.minecraft.resources.Identifier;
import net.minecraft.world.phys.Vec3;
import org.joml.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(WeatherEffectRenderer.class)
abstract class WeatherSceneMixin implements SceneEnvironmentSource {
    @Shadow @Final private GameRenderer gameRenderer;
    @Shadow @Final private TextureManager textureManager;
    @Shadow @Final private static Identifier RAIN_LOCATION,SNOW_LOCATION;
    @Shadow protected abstract void prepareInstances(VertexConsumer output,List<WeatherEffectRenderer.ColumnInstance> columns,Vec3 camera,float alpha,int radius,float intensity);
    @Unique private final ArrayList<SceneMesh> raynium$meshes=new ArrayList<>();
    @Override public void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view,boolean oit) {
        var weather=state.levelRenderState.weatherRenderState;
        int rain=weather.rainColumns.size()*4,snow=weather.snowColumns.size()*4;
        if(rain+snow==0) return;
        SceneMesh mesh=null;
        for(var item:raynium$meshes) if(!item.borrowed()) { mesh=item; break; }
        if(mesh==null) { mesh=new SceneMesh(); raynium$meshes.add(mesh); }
        var frame=TakeoverRuntime.frameBuffer().scene(); var camera=state.levelRenderState.cameraRenderState;
        TakeoverRuntime.retain(mesh);
        var builder=mesh.begin(PrimitiveTopology.QUADS,DefaultVertexFormat.PARTICLE);
        prepareInstances(builder,weather.rainColumns,camera.pos,1,weather.radius,weather.intensity);
        prepareInstances(builder,weather.snowColumns,camera.pos,.8f,weather.radius,weather.intensity);
        int vertices=mesh.finish(builder,frame);
        var uniforms=new SceneUniforms(frame,state,projection,view,net.minecraft.world.level.CardinalLighting.Type.DEFAULT);
        int transforms=uniforms.transforms(view,new Matrix4f());
        var lightSampler=RenderSystem.getSamplerCache().getClampToEdge(FilterMode.LINEAR);
        for(int type=0;type<2;type++) {
            int count=type==0?rain:snow; if(count==0) continue;
            long offset=type==0?0:(long)rain*DefaultVertexFormat.PARTICLE.getVertexSize();
            var texture=textureManager.getTexture(type==0?RAIN_LOCATION:SNOW_LOCATION);
            for(int stage=0;stage<(oit?3:1);stage++) {
                var pipeline=oit?RenderPipelines.OIT_WEATHER.getPipeline(OitStage.values()[stage]):RenderPipelines.WEATHER;
                var program=ScenePrograms.require(pipeline); int first=frame.bindingCount(); uniforms.bind(program,transforms);
                program.texture(frame,"Sampler0",texture.getTextureView(),texture.getSampler());
                program.texture(frame,"Sampler2",gameRenderer.lightmap(),lightSampler);
                if(oit) program.oitTextures(frame);
                frame.draw(program.id(),oit?NativeSceneFrame.OIT_BOUNDS+stage:NativeSceneFrame.AFTER_TERRAIN,
                    vertices,-1,count,0,0,first,1,-1,offset,0,0,0,0,state.windowRenderState.width,state.windowRenderState.height);
            }
        }
    }
    @Inject(method="close",at=@At("HEAD"))
    private void raynium$close(CallbackInfo ci) { for(var mesh:raynium$meshes) mesh.close(); raynium$meshes.clear(); }
}
