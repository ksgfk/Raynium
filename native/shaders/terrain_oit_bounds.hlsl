#define ALPHA_ONLY
#define ALPHA_CUTOFF 0.1
#include "terrain_common.hlsli"
#include "oit_common.hlsli"
[shader("vertex")]
Pixel VSMain(Vertex input) { return terrainVertex(input); }
[shader("pixel")]
float4 PSMain(Pixel input) : SV_Target0 {
    float alpha=terrainColor(input).a,depth=deviceToLinearDepth(input.position.z);
    return float4(-depth,depth,input.position.z,alpha>0.99?input.position.z:0);
}
