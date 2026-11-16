package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.device.GpuDevice;
import dev.raynium.client.takeover.scene.*;
import java.nio.ByteBuffer;
import java.util.function.Supplier;
import net.minecraft.client.renderer.*;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.texture.*;
import net.minecraft.util.Mth;
import net.minecraft.world.level.dimension.DimensionType.Skybox;
import org.joml.*;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.At;

@Mixin(SkyRenderer.class)
abstract class SkySceneMixin implements SceneSky.Source {
    @Shadow @Final private TextureAtlas celestialsAtlas;
    @Shadow @Final private AbstractTexture endSkyTexture;
    @Shadow @Final private GpuBuffer starBuffer,topSkyBuffer,bottomSkyBuffer,endSkyBuffer,sunBuffer,moonBuffer,sunriseBuffer,endFlashBuffer;
    @Shadow private int starIndexCount;
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/blaze3d/systems/RenderSystem;getDevice()Lcom/mojang/renderpearl/api/device/GpuDevice;"))
    private static GpuDevice raynium$device(Operation<GpuDevice> original) { return null; }
    @WrapOperation(method={"<init>","*"},at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createBuffer(Ljava/util/function/Supplier;ILjava/nio/ByteBuffer;)Lcom/mojang/renderpearl/api/buffers/GpuBuffer;"))
    private static GpuBuffer raynium$buffer(GpuDevice device,Supplier<String> name,int usage,ByteBuffer bytes,Operation<GpuBuffer> original) {
        return new SceneGeometry(usage,bytes);
    }
    @Override public void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view) {
        var sky=state.levelRenderState.skyRenderState;
        if(sky.skybox==Skybox.NONE) return;
        var scene=new SceneSky(state,projection,view); var white=new Vector4f(1);
        if(sky.skybox==Skybox.END) {
            scene.draw(RenderPipelines.END_SKY,endSkyBuffer,24,0,view,white,endSkyTexture);
            if(sky.endFlashIntensity>1e-5f) {
                var matrix=new Matrix4f(view).rotateY((180-sky.endFlashYAngle)*(float)(java.lang.Math.PI/180))
                    .rotateX((-90-sky.endFlashXAngle)*(float)(java.lang.Math.PI/180)).translate(0,100,0).scale(60,1,60);
                scene.draw(RenderPipelines.CELESTIAL,endFlashBuffer,4,0,matrix,new Vector4f(sky.endFlashIntensity),celestialsAtlas);
            }
            return;
        }
        scene.draw(RenderPipelines.SKY,topSkyBuffer,10,0,view,new Vector4f(sky.skyColor,1),null);
        float alpha=sky.sunriseAndSunsetColor.w();
        if(alpha>.001f) {
            var matrix=new Matrix4f(view).rotateX((float)(java.lang.Math.PI/2))
                .rotateZ((Mth.sin(sky.sunAngle)<0?270:90)*(float)(java.lang.Math.PI/180)).scale(1,1,alpha);
            scene.draw(RenderPipelines.SUNRISE_SUNSET,sunriseBuffer,18,0,matrix,sky.sunriseAndSunsetColor,null);
        }
        var celestial=new Matrix4f(view).rotateY((float)(-java.lang.Math.PI/2));
        var color=new Vector4f(1,1,1,sky.rainBrightness);
        scene.draw(RenderPipelines.CELESTIAL,sunBuffer,4,0,new Matrix4f(celestial).rotateX(sky.sunAngle).translate(0,100,0).scale(30,1,30),color,celestialsAtlas);
        scene.draw(RenderPipelines.CELESTIAL,moonBuffer,4,sky.moonPhase.index()*80L,new Matrix4f(celestial).rotateX(sky.moonAngle).translate(0,100,0).scale(20,1,20),color,celestialsAtlas);
        if(sky.starBrightness>0) scene.draw(RenderPipelines.STARS,starBuffer,starIndexCount/6*4,0,new Matrix4f(celestial).rotateX(sky.starAngle),new Vector4f(sky.starBrightness),null);
        if(sky.shouldRenderDarkDisc) scene.draw(RenderPipelines.SKY,bottomSkyBuffer,10,0,new Matrix4f(view).translate(0,12,0),new Vector4f(0,0,0,1),null);
    }
}
