#define ALPHA_CUTOFF 0.1
#define OIT_BOUNDS_INPUT
#define OIT_COEFFICIENT_INPUT
#include "terrain_common.hlsli"
#include "oit_common.hlsli"
[shader("vertex")]
Pixel VSMain(Vertex input) { return terrainVertex(input); }
[shader("pixel")]
float4 PSMain(Pixel input) : SV_Target0 {
    float4 color=terrainColor(input);
    int2 pos=int2(input.position.xy);
    float transmittance=sampleTransmittance(pos,normalizeDepth(pos,input.position.z),1-color.a);
    color=float4(color.rgb*color.a,color.a)*transmittance;
    return applyFog(input,color,true);
}
