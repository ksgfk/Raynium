#include "terrain_common.hlsli"
[shader("vertex")]
Pixel VSMain(Vertex input) { return terrainVertex(input); }
[shader("pixel")]
float4 PSMain(Pixel input) : SV_Target0 { return applyFog(input,terrainColor(input),false); }
