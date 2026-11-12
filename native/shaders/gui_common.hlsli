[[vk::binding(0, 0)]] cbuffer GuiFrame : register(b0, space0) {
    float4 GuiViewport;
};
#ifdef GUI_TEXTURED
[[vk::binding(0, 1)]] Texture2D<float4> GuiTexture : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState GuiSampler : register(s0, space1);
#endif
struct GuiVertex {
    float3 position : POSITION;
#ifdef GUI_TEXTURED
    float2 uv : TEXCOORD0;
#endif
    float4 color : COLOR;
};
struct GuiPixel {
    float4 position : SV_Position;
#ifdef GUI_TEXTURED
    float2 uv : TEXCOORD0;
#endif
    float4 color : COLOR;
};
[shader("vertex")]
GuiPixel VSMain(GuiVertex input) {
    GuiPixel output;
    output.position=float4(input.position.x/GuiViewport.x*2-1,(1-input.position.y/GuiViewport.y*2)*GuiViewport.z,0,1);
#ifdef GUI_TEXTURED
    output.uv=input.uv;
#endif
    output.color=input.color;
    return output;
}
[shader("pixel")]
float4 PSMain(GuiPixel input) : SV_Target0 {
    float4 color=input.color;
#ifdef GUI_TEXTURED
    float4 texColor=GuiTexture.Sample(GuiSampler,input.uv);
#ifdef GUI_TEXT
    if(GuiViewport.w!=0) texColor=texColor.rrrr;
#endif
    color*=texColor;
#endif
#ifdef GUI_TEXT
    if(color.a<0.1) discard;
#else
    if(color.a==0) discard;
#endif
    return color;
}
