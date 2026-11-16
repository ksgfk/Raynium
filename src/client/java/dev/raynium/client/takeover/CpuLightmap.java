package dev.raynium.client.takeover;

import net.minecraft.client.renderer.state.LightmapRenderState;

public final class CpuLightmap {
    private CpuLightmap() {}
    public static byte[] pixels(LightmapRenderState state) {
        byte[] pixels=new byte[16*16*4];
        write(state,pixels);
        return pixels;
    }
    public static void write(LightmapRenderState state,byte[] pixels) {
        if(pixels.length!=16*16*4) throw new IllegalArgumentException("Lightmap needs 16 by 16 RGBA pixels");
        for(int sky=0;sky<16;sky++) for(int block=0;block<16;block++) {
            float level=block/15f,curve=2*level-1,white=.9f*curve*curve;
            float blockBrightness=brightness(level)*state.blockFactor,skyBrightness=brightness(sky/15f)*state.skyFactor;
            float red=channel(state,0,white,blockBrightness,skyBrightness);
            float green=channel(state,1,white,blockBrightness,skyBrightness);
            float blue=channel(state,2,white,blockBrightness,skyBrightness);
            float max=Math.max(red,Math.max(green,blue)),inverted=1-max;
            float scale=max>0?(1-inverted*inverted*inverted*inverted)/max:0;
            int offset=(sky*16+block)*4;
            float gamma=1+state.brightness*(scale-1);
            pixels[offset]=(byte)Math.round(Math.clamp(red*gamma,0,1)*255);
            pixels[offset+1]=(byte)Math.round(Math.clamp(green*gamma,0,1)*255);
            pixels[offset+2]=(byte)Math.round(Math.clamp(blue*gamma,0,1)*255);
            pixels[offset+3]=(byte)255;
        }
    }
    private static float channel(LightmapRenderState state,int channel,float white,float block,float sky) {
        float value=Math.max(state.ambientColor.get(channel),state.nightVisionColor.get(channel)*state.nightVisionEffectIntensity);
        value+=state.skyLightColor.get(channel)*sky;
        value+=(state.blockLightTint.get(channel)*(1-white)+white)*block;
        value*=1-state.bossOverlayWorldDarkening*(channel==0?.3f:.4f);
        return Math.clamp(value-state.darknessEffectScale,0,1);
    }
    private static float brightness(float level) { return level/(4-3*level); }
}
