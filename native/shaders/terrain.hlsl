[[vk::binding(0, 0)]] cbuffer Frame : register(b0) {
    float4x4 Projection;
    float4x4 View;
    int4 CameraBlock;
    float4 CameraFraction;
    float4 FogColor;
    float4 FogDistances;
};
[[vk::binding(1, 0)]] Texture2D<float4> Atlas : register(t0);
[[vk::binding(2, 0)]] Texture2D<float4> Lightmap : register(t1);
[[vk::binding(3, 0)]] SamplerState AtlasSampler : register(s0);
[[vk::binding(4, 0)]] SamplerState LightSampler : register(s1);
struct Vertex {
    float3 position : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    uint2 light : TEXCOORD1;
    int3 origin : TEXCOORD2;
    float visibility : TEXCOORD3;
};
struct Pixel {
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float3 fog : TEXCOORD1;
};
Pixel VSMain(Vertex input) {
    Pixel output;
    float3 position = input.position + (input.origin - CameraBlock.xyz) - CameraFraction.xyz;
    output.position = mul(Projection, mul(View, float4(position, 1)));
    output.position.y *= CameraFraction.w;
    float2 lightUV = clamp(input.light / 256.0 + 0.5 / 16.0, 0.5 / 16.0, 15.5 / 16.0);
    output.color = input.color * Lightmap.SampleLevel(LightSampler, lightUV, 0);
    output.uv = input.uv;
    output.fog = float3(length(position), max(length(position.xz), abs(position.y)),
        lerp(1, input.visibility, saturate((length(position) - 16) / 16)));
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
float4 PSMain(Pixel input) : SV_Target0 {
    float4 color = sampleAtlas(input.uv) * input.color;
    color.rgb = lerp(FogColor.rgb, color.rgb, input.fog.z);
    float fog = max(linearFog(input.fog.x, FogDistances.x, FogDistances.y),
                    linearFog(input.fog.y, FogDistances.z, FogDistances.w));
    color.rgb = lerp(color.rgb, FogColor.rgb, fog * FogColor.a);
    return color;
}
