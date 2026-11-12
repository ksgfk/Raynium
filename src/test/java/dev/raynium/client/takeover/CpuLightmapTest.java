package dev.raynium.client.takeover;

import net.minecraft.client.renderer.state.LightmapRenderState;
import org.joml.Vector3f;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class CpuLightmapTest {
    private static int channel(byte[] pixels,int block,int sky,int channel) { return pixels[(sky*16+block)*4+channel]&255; }
    @Test void blackSkyBlockAndBrightnessFollowShaderCoordinates() {
        var state=new LightmapRenderState(); state.ambientColor=new Vector3f();
        var black=CpuLightmap.pixels(state);
        assertEquals(0,channel(black,0,0,0)); assertEquals(255,channel(black,0,0,3));
        state.blockFactor=1; state.blockLightTint=new Vector3f(1,0,0);
        var block=CpuLightmap.pixels(state);
        assertEquals(255,channel(block,15,0,0)); assertEquals(230,channel(block,15,0,1));
        assertEquals(0,channel(block,0,15,0));
        state.blockFactor=0; state.skyFactor=.5f; state.skyLightColor=new Vector3f(0,1,0);
        var sky=CpuLightmap.pixels(state);
        assertEquals(128,channel(sky,0,15,1)); assertEquals(0,channel(sky,15,0,1));
        state.brightness=1;
        assertEquals(239,channel(CpuLightmap.pixels(state),0,15,1));
    }
    @Test void nightVisionAndDarknessUsePerChannelColor() {
        var state=new LightmapRenderState(); state.ambientColor=new Vector3f(.1f,.2f,.3f);
        state.nightVisionColor=new Vector3f(.8f,.4f,.2f); state.nightVisionEffectIntensity=.5f; state.darknessEffectScale=.1f;
        var pixels=CpuLightmap.pixels(state);
        assertEquals(77,channel(pixels,0,0,0)); assertEquals(26,channel(pixels,0,0,1)); assertEquals(51,channel(pixels,0,0,2));
    }
}
