struct ScreenPixel { float4 position:SV_Position; };
struct ScreenOutput { float4 color:SV_Target0; float depth:SV_Depth; };
ScreenPixel screenVertex(uint id) {
    ScreenPixel output; output.position=float4(id==1?3:-1,id==2?3:-1,0,1); return output;
}
