#include <core/platform.hlsli>
struct Dimensions { float Width, Height; };
VK_PUSH_CONSTANT ConstantBuffer<Dimensions> Size : register(b0);

[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, 0.5, 1.0);
}

[shader("pixel")]
float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    // MC's final surface blit flips Y; UV origin is the displayed rectangle's top-left.
    float2 uv = position.xy / float2(Size.Width, Size.Height);
    return float4(uv.x, 1.0 - uv.y, 0.0, 1.0);
}
