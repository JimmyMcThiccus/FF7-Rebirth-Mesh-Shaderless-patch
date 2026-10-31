// Pixel shader reading TEXCOORD1 (float3) instead of TEXCOORD0: paired with ms_simple it keeps
// position + a 3-component output, so the vertex is 32 bytes and the converter uses 16-byte units.
struct VOut { float4 pos : SV_Position; float3 c : TEXCOORD1; };

float4 main(VOut i) : SV_Target
{
    return float4(frac(i.c.xy / 16), i.c.z / 9, 1);
}
