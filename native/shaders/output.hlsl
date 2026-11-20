#include "screen.hlsli"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor : register(t0, space0);
[[vk::binding(1, 0)]] cbuffer OutputParameters : register(b1, space0) { int4 OutputRegion; };
[shader("vertex")]
ScreenPixel VSMain(uint id:SV_VertexID) { return screenVertex(id); }
[shader("pixel")]
float4 PSMain(ScreenPixel input):SV_Target0 {
    int2 pixel = int2(input.position.xy);
    if (OutputRegion.x > 0) pixel.y = OutputRegion.x - 1 - pixel.y;
    return SceneColor.Load(int3(pixel,0));
}
