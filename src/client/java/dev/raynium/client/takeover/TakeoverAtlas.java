package dev.raynium.client.takeover;

import dev.raynium.bridge.NativeTexture;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.renderer.texture.*;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;

public final class TakeoverAtlas {
    private final NativeTexture texture;
    private final List<Animation> animations=new ArrayList<>();
    private static final class Animation {
        final TextureAtlasSprite sprite;
        int frame,subFrame;
        Animation(TextureAtlasSprite sprite) { this.sprite=sprite; }
        boolean tick() {
            var info=sprite.contents().animatedTexture;
            var current=info.frames.get(frame);
            subFrame++;
            boolean changed=false;
            if(subFrame>=current.time()) {
                frame=(frame+1)%info.frames.size(); subFrame=0;
                changed=current.index()!=info.frames.get(frame).index();
            }
            return changed || info.interpolateFrames;
        }
    }
    public TakeoverAtlas(TextureAtlas atlas,List<TextureAtlasSprite> sprites) {
        texture=((TakeoverHandles.Texture)atlas.getTexture()).nativeTexture();
        for(var sprite:sprites) if(sprite.isAnimated()) animations.add(new Animation(sprite));
        for(int mip=0;mip<texture.mipLevels();mip++) {
            int width=Math.max(1,texture.width()>>mip),height=Math.max(1,texture.height()>>mip);
            byte[] bytes=new byte[Math.multiplyExact(Math.multiplyExact(width,height),4)];
            var target=MemorySegment.ofArray(bytes);
            for(var sprite:sprites) {
                var animation=sprite.contents().animatedTexture;
                int first=animation==null?0:animation.frames.getFirst().index();
                stitch(sprite,mip,first,first,0,target,width,sprite.getX()>>mip,sprite.getY()>>mip);
            }
            texture.upload(mip,0,0,width,height,bytes);
        }
    }
    public void tick() {
        try(var zone=com.mojang.jtracy.TracyClient.beginZone("Raynium atlas animation",false)) {
            animate();
        }
    }
    private void animate() {
        for(var animation:animations) {
            if(!animation.tick()) continue;
            var sprite=animation.sprite;
            var info=sprite.contents().animatedTexture;
            int current=info.frames.get(animation.frame).index(),next=info.frames.get((animation.frame+1)%info.frames.size()).index();
            float progress=info.interpolateFrames?(int)(((float)animation.subFrame/info.frames.get(animation.frame).time())*1000)/1000f:0;
            for(int mip=0;mip<texture.mipLevels();mip++) {
                int width=(sprite.contents().width()+2*sprite.padding)>>mip,height=(sprite.contents().height()+2*sprite.padding)>>mip;
                byte[] bytes=new byte[Math.multiplyExact(Math.multiplyExact(width,height),4)];
                stitch(sprite,mip,current,next,progress,MemorySegment.ofArray(bytes),width,0,0);
                texture.upload(mip,sprite.getX()>>mip,sprite.getY()>>mip,width,height,bytes);
            }
        }
    }
    private static void stitch(TextureAtlasSprite sprite,int mip,int frame,int next,float progress,MemorySegment destination,int pitch,int x,int y) {
        var contents=sprite.contents();
        var image=contents.byMipLevel[mip];
        var source=MemorySegment.ofBuffer(image.getPixelBytes());
        int width=Math.max(1,contents.width()>>mip),height=Math.max(1,contents.height()>>mip),padding=sprite.padding>>mip;
        int tileWidth=(contents.width()+2*sprite.padding)>>mip,tileHeight=(contents.height()+2*sprite.padding)>>mip;
        int columns=contents.animatedTexture==null?1:contents.animatedTexture.frameRowSize;
        int fx=(frame%columns)*width,fy=(frame/columns)*height,nx=(next%columns)*width,ny=(next/columns)*height;
        for(int row=0;row<tileHeight;row++) {
            int sy=Math.clamp(row-padding,0,height-1);
            for(int column=0;column<tileWidth;column++) {
                int sx=Math.clamp(column-padding,0,width-1);
                int color=source.get(JAVA_INT_UNALIGNED,((long)(fy+sy)*image.getWidth()+fx+sx)*4);
                if(progress>0) {
                    int other=source.get(JAVA_INT_UNALIGNED,((long)(ny+sy)*image.getWidth()+nx+sx)*4),mixed=0;
                    for(int shift=0;shift<32;shift+=8) { int a=(color>>>shift)&255,b=(other>>>shift)&255; mixed|=Math.round(a+(b-a)*progress)<<shift; }
                    color=mixed;
                }
                destination.set(JAVA_INT_UNALIGNED,((long)(y+row)*pitch+x+column)*4,color);
            }
        }
    }
}
