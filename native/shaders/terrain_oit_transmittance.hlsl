#define ALPHA_ONLY
#define ALPHA_CUTOFF 0.1
#define OIT_BOUNDS_INPUT
#include "terrain_common.hlsli"
#include "oit_common.hlsli"
struct Coefficients { float4 a:SV_Target0; float4 b:SV_Target1; };
[shader("vertex")]
Pixel VSMain(Vertex input) { return terrainVertex(input); }
[shader("pixel")]
Coefficients PSMain(Pixel input) {
    float absorbance=toAbsorbance(1-terrainColor(input).a);
    float original=normalizeDepth(int2(input.position.xy),input.position.z),depth=original*7/8;
    float c[8]={absorbance*(1-depth),0,0,0,0,0,0,0};
    int tree=int(floor(original*7))+8;
    for(int level=0;level<=2;level++) {
        int power=2-level,parent=tree>>1,right=tree&1,sign=1-(right<<1);
        float width=exp2(-float(power)),height=exp2(power*0.5);
        float offset=depth-width*float(parent&((1<<power)-1));
        c[parent]=(right*width+sign*offset)*height*absorbance; tree=parent;
    }
    Coefficients output; output.a=float4(c[0],c[1],c[2],c[3]); output.b=float4(c[4],c[5],c[6],c[7]);
    return output;
}
