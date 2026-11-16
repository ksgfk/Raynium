#include "screen.hlsli"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor : register(t0, space0);
[shader("vertex")]
ScreenPixel VSMain(uint id:SV_VertexID) { return screenVertex(id); }
[shader("pixel")]
float4 PSMain(ScreenPixel input):SV_Target0 { return SceneColor.Load(int3(int2(input.position.xy),0)); }
