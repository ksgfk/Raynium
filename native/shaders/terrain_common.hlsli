#include "frame.hlsli"
[[vk::binding(1, 1)]] Texture2D<float4> Atlas : register(t0, space1);
[[vk::binding(3, 1)]] SamplerState AtlasSampler : register(s0, space1);
#ifndef ALPHA_ONLY
[[vk::binding(2, 1)]] Texture2D<float4> Lightmap : register(t1, space1);
[[vk::binding(4, 1)]] SamplerState LightSampler : register(s1, space1);
#endif
struct Vertex {
    float3 position : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
#ifndef ALPHA_ONLY
    uint2 light : TEXCOORD1;
#endif
    int3 origin : TEXCOORD2;
#ifndef ALPHA_ONLY
    float visibility : TEXCOORD3;
#endif
};
struct Pixel {
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
#ifndef ALPHA_ONLY
    float3 fog : TEXCOORD1;
#endif
};
Pixel terrainVertex(Vertex input) {
    Pixel output;
    float3 position = input.position + (input.origin - CameraBlock.xyz) - CameraFraction.xyz;
    output.position = mul(Projection, mul(View, float4(position, 1)));
    output.position.y *= CameraFraction.w;
#ifdef ALPHA_ONLY
    output.color = input.color;
#else
    float2 lightUV = clamp(input.light / 256.0 + 0.5 / 16.0, 0.5 / 16.0, 15.5 / 16.0);
    output.color = input.color * Lightmap.SampleLevel(LightSampler, lightUV, 0);
    output.fog = float3(length(position), max(length(position.xz), abs(position.y)),
        lerp(1, input.visibility, saturate((length(position) - 16) / 16)));
#endif
    output.uv = input.uv;
    return output;
}
float linearFog(float distance, float start, float end) {
    return distance <= start ? 0 : distance >= end ? 1 : (distance - start) / (end - start);
}
float4 sampleAtlas(float2 uv) {
    uint width,height; Atlas.GetDimensions(width,height);
    float2 pixel=1.0/float2(width,height),du=ddx(uv),dv=ddy(uv);
    float2 screen=max(sqrt(du*du+dv*dv),1e-20);
    float2 texel=uv/pixel,center=round(texel)-0.5;
    float2 offset=saturate((texel-center-0.5)*pixel/screen+0.5);
    float4 nearest=Atlas.SampleGrad(AtlasSampler,(center+offset)*pixel,du,dv);
    if(CameraBlock.w==0) return nearest;
    float minimumPixel=min(pixel.x,pixel.y);
    float lod=max(0,log2(max(1e-20,sqrt(length(du)*length(dv)))/minimumPixel));
    const float2 offsets[4]={float2(.125,.375),float2(-.125,-.375),float2(.375,-.125),float2(-.375,.125)};
    float4 low=0,high=0;
    for(int i=0;i<4;i++) {
        low+=Atlas.SampleLevel(AtlasSampler,uv+offsets[i]*pixel,floor(lod));
        high+=Atlas.SampleLevel(AtlasSampler,uv+offsets[i]*pixel,floor(lod)+1);
    }
    return lerp(nearest,lerp(low,high,frac(lod))*.25,smoothstep(minimumPixel,2*minimumPixel,max(screen.x,screen.y)));
}
float4 terrainColor(Pixel input) {
    float4 color=sampleAtlas(input.uv)*input.color;
#ifndef ALPHA_ONLY
    color=lerp(FogColor*float4(1,1,1,color.a),color,input.fog.z);
#endif
#ifdef ALPHA_CUTOFF
    if(color.a<ALPHA_CUTOFF) discard;
#endif
    return color;
}
#ifndef ALPHA_ONLY
float4 applyFog(Pixel input,float4 color,bool premultiplied) {
    float fog=max(linearFog(input.fog.x,FogDistances.x,FogDistances.y),
                  linearFog(input.fog.y,FogDistances.z,FogDistances.w));
    color.rgb=lerp(color.rgb,FogColor.rgb*(premultiplied?color.a:1),fog*FogColor.a);
    return color;
}
#endif
