// Test mesh shader shaped like FF7 Rebirth's: 128 threads, thread t writes only vertex t and primitive t,
// no groupshared memory, so the converter's VS mode (mesh shader run as the vertex shader) applies.
// Same 8x8-vertex grid patches as ms_simple, so the image matches ms_simple's.
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 unused : TEXCOORD1; };
cbuffer CB : register(b0) { float4 xform; uint groupsX; };   // xy scale, zw offset

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
          out vertices VOut verts[64], out indices uint3 tris[98])
{
    uint numTris = (gid.x & 1) ? 90 : 98;
    SetMeshOutputCounts(64, numTris);
    if (gtid < 64) {
        float2 p = float2(gid.x * 7 + (gtid % 8), gid.y * 7 + (gtid / 8));
        verts[gtid].pos = float4(p * xform.xy + xform.zw, 0.5, 1);
        verts[gtid].uv = p / 64.0;
        verts[gtid].unused = float3(p, gid.x);
    }
    if (gtid < numTris) {
        uint q = gtid / 2, x = q % 7, y = q / 7, v = y * 8 + x;
        tris[gtid] = (gtid & 1) ? uint3(v + 1, v + 9, v + 8) : uint3(v, v + 1, v + 8);
    }
}
