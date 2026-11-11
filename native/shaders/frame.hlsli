[[vk::binding(0, 0)]] cbuffer Frame : register(b0, space0) {
    float4x4 Projection;
    float4x4 View;
    int4 CameraBlock;
    float4 CameraFraction;
    float4 FogColor;
    float4 FogDistances;
};
