float deviceToLinearDepth(float depth) { return Projection[2][3]/(depth+Projection[2][2]); }
float toAbsorbance(float transmittance) { return clamp(-log(max(transmittance,0.0001)),0,4); }
float toTransmittance(float absorbance) { return clamp(exp(-absorbance),0.0001,1); }
#ifdef OIT_BOUNDS_INPUT
[[vk::binding(0, 2)]] Texture2D<float4> DepthBounds : register(t0, space2);
float normalizeDepth(int2 pos,float deviceDepth) {
    float4 bounds=DepthBounds.Load(int3(pos,0));
    float range=max(bounds.g+bounds.r,0.0001);
    float depth=clamp(deviceToLinearDepth(deviceDepth)+bounds.r,0,range);
    if(0.75*range<=10) return depth/range;
    return depth<=10 ? depth/10*0.75 : 0.75+(depth-10)/(range-10)*0.25;
}
#endif
#ifdef OIT_COEFFICIENT_INPUT
[[vk::binding(1, 2)]] Texture2D<float4> Coeff0 : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D<float4> Coeff1 : register(t2, space2);
float sampleTransmittance(int2 pos,float originalDepth,float currentTransmittance) {
    float4 c0=Coeff0.Load(int3(pos,0)),c1=Coeff1.Load(int3(pos,0));
    float coefficients[8]={c0.x,c0.y,c0.z,c0.w,c1.x,c1.y,c1.z,c1.w};
    if(coefficients[0]==0) return 1;
    float measured=originalDepth*7,depth=measured/8;
    float absorbance=toAbsorbance(currentTransmittance);
    float average=coefficients[0]-absorbance*(1-depth);
    int b=clamp(int(floor(measured)),0,7);
    bool sampleA=b>=1;
    int a=(sampleA?b-1:b)+8; b+=8;
    float sb=average,sa=sampleA?average:0;
    for(int level=0;level<=2;level++) {
        int power=2-level,parentB=b>>1,rightB=b&1,signB=1-(rightB<<1);
        float width=exp2(-float(power)),height=exp2(power*0.5);
        float offset=depth-width*float(parentB&((1<<power)-1));
        float cb=coefficients[parentB]-(rightB*width+signB*offset)*height*absorbance;
        sb-=height*cb*signB; b=parentB;
        if(sampleA) {
            int parentA=a>>1,signA=1-((a&1)<<1);
            sa-=height*(parentA==parentB?cb:coefficients[parentA])*signA; a=parentA;
        }
    }
    return toTransmittance(lerp(sa,sb,measured>=8?1:frac(measured)));
}
#endif
