package dev.raynium.client.takeover;

import net.minecraft.client.renderer.state.LightmapRenderState;

public final class CpuLightmap {
    private CpuLightmap() {}
    public static byte[] pixels(LightmapRenderState state) {
        byte[] pixels=new byte[16*16*4];
        for(int sky=0;sky<16;sky++) for(int block=0;block<16;block++) {
            float level=block/15f,curve=2*level-1,white=.9f*curve*curve;
            float blockBrightness=brightness(level)*state.blockFactor,skyBrightness=brightness(sky/15f)*state.skyFactor;
            float[] color=new float[3];
            for(int c=0;c<3;c++) {
                float value=Math.max(state.ambientColor.get(c),state.nightVisionColor.get(c)*state.nightVisionEffectIntensity);
                value+=state.skyLightColor.get(c)*skyBrightness;
                value+=(state.blockLightTint.get(c)*(1-white)+white)*blockBrightness;
                value*=1-state.bossOverlayWorldDarkening*(c==0?.3f:.4f);
                color[c]=Math.clamp(value-state.darknessEffectScale,0,1);
            }
            float max=Math.max(color[0],Math.max(color[1],color[2])),inverted=1-max;
            float scale=max>0?(1-inverted*inverted*inverted*inverted)/max:0;
            int offset=(sky*16+block)*4;
            for(int c=0;c<3;c++) pixels[offset+c]=(byte)Math.round(Math.clamp(color[c]*(1+state.brightness*(scale-1)),0,1)*255);
            pixels[offset+3]=(byte)255;
        }
        return pixels;
    }
    private static float brightness(float level) { return level/(4-3*level); }
}
