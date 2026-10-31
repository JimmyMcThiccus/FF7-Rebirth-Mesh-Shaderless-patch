struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VOut i) : SV_Target
{
    return float4(frac(i.uv * 4), 0.5, 1);
}
