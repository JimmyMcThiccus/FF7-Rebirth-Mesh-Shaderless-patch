// Placeholder vertex shader for mesh pipelines that have no converted shaders yet:
// every vertex gets a NaN position, so nothing is rasterized.
float4 main(uint id : SV_VertexID) : SV_Position
{
    return asfloat(0x7fc00000u).xxxx;
}
