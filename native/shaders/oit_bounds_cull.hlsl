#define OIT_BOUNDS_INPUT
#include "frame.hlsli"
#include "oit_common.hlsli"
#include "screen.hlsli"
[shader("vertex")]
ScreenPixel VSMain(uint id:SV_VertexID) { return screenVertex(id); }
[shader("pixel")]
ScreenOutput PSMain(ScreenPixel input) {
    float4 bounds=DepthBounds.Load(int3(int2(input.position.xy),0));
    ScreenOutput output;
    output.color=float4(bounds.r,min(bounds.g,deviceToLinearDepth(bounds.a)),bounds.b,bounds.a);
    output.depth=bounds.a; return output;
}
