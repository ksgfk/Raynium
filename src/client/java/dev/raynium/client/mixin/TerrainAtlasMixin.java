package dev.raynium.client.mixin;
import dev.raynium.client.terrain.*;
import net.minecraft.client.renderer.texture.*;
import java.util.List;
import org.spongepowered.asm.mixin.*;
import org.spongepowered.asm.mixin.injection.*;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(TextureAtlas.class)
abstract class TerrainAtlasMixin {
    @Shadow private List<TextureAtlasSprite> sprites;
    @Shadow private List<SpriteContents.AnimationState> animatedTexturesStates;
    @Inject(method="upload",at=@At("RETURN"))
    private void raynium$atlas(CallbackInfo ci) { if(TerrainCapture.ENABLED) TerrainTextures.uploaded((TextureAtlas)(Object)this); }
    @Inject(method="uploadAnimationFrames",at=@At("HEAD"))
    private void raynium$dirty(CallbackInfo ci) {
        var atlas=(TextureAtlas)(Object)this;
        if(!TerrainCapture.active() || !atlas.location().equals(TextureAtlas.LOCATION_BLOCKS)) return;
        int index=0;
        for(var sprite:sprites) if(sprite.isAnimated()) {
            if(index>=animatedTexturesStates.size()) break;
            if(animatedTexturesStates.get(index++).needsToDraw()) {
                int padding=Math.round(sprite.getU0()*atlas.getTexture().getWidth(0))-sprite.getX();
                TerrainTextures.dirty(new TerrainTextures.Rect(sprite.getX(),sprite.getY(),sprite.contents().width()+padding*2,sprite.contents().height()+padding*2));
            }
        }
    }
}
