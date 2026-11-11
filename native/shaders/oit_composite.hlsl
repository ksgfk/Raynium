#define OIT_BOUNDS_INPUT
#define OIT_COEFFICIENT_INPUT
#include "frame.hlsli"
#include "oit_common.hlsli"
#include "screen.hlsli"
[[vk::binding(3, 2)]] Texture2D<float4> Accumulation : register(t3, space2);
[shader("vertex")]
ScreenPixel VSMain(uint id:SV_VertexID) { return screenVertex(id); }
[shader("pixel")]
ScreenOutput PSMain(ScreenPixel input) {
    int2 pos=int2(input.position.xy);
    float4 color=Accumulation.Load(int3(pos,0));
    float transmittance=sampleTransmittance(pos,100000,1);
    float coverage=1-(transmittance<0.02?0:transmittance);
    if(coverage<0.00001 && dot(color.rgb,float3(1,1,1))<0.00001) discard;
    ScreenOutput output;
    output.color=float4(color.rgb*(color.a>0.00001?coverage/color.a:1),coverage);
    output.depth=DepthBounds.Load(int3(pos,0)).b; return output;
}
