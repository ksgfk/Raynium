package dev.raynium.client.mixin.takeover;

import com.llamalad7.mixinextras.injector.wrapoperation.*;
import com.mojang.blaze3d.platform.Lighting;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.device.GpuDevice;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import dev.raynium.client.takeover.*;
import dev.raynium.client.takeover.scene.SceneOffscreen;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.render.GuiItemAtlas;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.client.renderer.texture.OverlayTexture;
import org.joml.Vector4fc;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GuiItemAtlas.class)
abstract class GuiItemAtlasSceneMixin {
    @Shadow @Final private FeatureRenderDispatcher featureRenderDispatcher;
    @Shadow @Final private SubmitNodeStorage submitNodeStorage;
    @Shadow @Final private PoseStack poseStack;
    @Shadow @Final private GpuTexture texture;
    @Shadow @Final private int slotTextureSize,textureSize;
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createTexture(Ljava/lang/String;ILcom/mojang/renderpearl/api/GpuFormat;IIII)Lcom/mojang/renderpearl/api/textures/GpuTexture;"))
    private GpuTexture raynium$texture(GpuDevice device,String label,int usage,GpuFormat format,int width,int height,int layers,int mips,Operation<GpuTexture> original) {
        return format==GpuFormat.D32_FLOAT?new SceneOffscreen.Depth(width,height):TakeoverHandles.texture(label,usage,format,width,height,layers,mips);
    }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/device/GpuDevice;createCommandEncoder()Lcom/mojang/renderpearl/api/commands/CommandEncoder;"))
    private CommandEncoder raynium$encoder(GpuDevice device,Operation<CommandEncoder> original) { return null; }
    @WrapOperation(method="<init>",at=@At(value="INVOKE",target="Lcom/mojang/renderpearl/api/commands/CommandEncoder;clearColorAndDepthTextures(Lcom/mojang/renderpearl/api/textures/GpuTexture;Lorg/joml/Vector4fc;Lcom/mojang/renderpearl/api/textures/GpuTexture;D)V"))
    private void raynium$clear(CommandEncoder encoder,GpuTexture color,Vector4fc clear,GpuTexture depth,double depthValue,Operation<Void> original) {}
    @Inject(method="drawToSlot",at=@At("HEAD"),cancellable=true)
    private void raynium$draw(int x,int y,boolean clear,ItemStackRenderState item,CallbackInfo ci) {
        poseStack.pushPose();
        try {
            poseStack.translate(slotTextureSize/2f,slotTextureSize/2f,0); poseStack.scale(slotTextureSize,-slotTextureSize,slotTextureSize);
            Minecraft.getInstance().gameRenderer.lighting().setupFor(item.usesBlockLight()?Lighting.Entry.ITEMS_3D:Lighting.Entry.ITEMS_FLAT);
            item.submit(poseStack,submitNodeStorage,15728880,OverlayTexture.NO_OVERLAY,0);
            SceneOffscreen.capture(texture,x*slotTextureSize,textureSize-(y+1)*slotTextureSize,slotTextureSize,slotTextureSize,
                submitNodeStorage,featureRenderDispatcher,RenderSystem.getModelViewStack());
        } finally { poseStack.popPose(); }
        ci.cancel();
    }
}
