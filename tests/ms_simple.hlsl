// Test mesh shader: each group emits a 8x8 vertex grid patch (98 triangles) of a larger
// grid, so coverage and per-vertex attributes are easy to compare against a reference.
// unused: written by the mesh shader but never read by ps_simple (the converter should drop it).
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 unused : TEXCOORD1; };
cbuffer CB : register(b0) { float4 xform; uint groupsX; };   // xy scale, zw offset

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
          out vertices VOut verts[64], out indices uint3 tris[98])
{
    // Leave a few triangles unused in odd groups to exercise numPrims < maxPrims.
    uint numTris = (gid.x & 1) ? 90 : 98;
    SetMeshOutputCounts(64, numTris);
    for (uint i = gtid; i < 64; i += 32) {
        float2 p = float2(gid.x * 7 + (i % 8), gid.y * 7 + (i / 8));
        verts[i].pos = float4(p * xform.xy + xform.zw, 0.5, 1);
        verts[i].uv = p / 64.0;
        verts[i].unused = float3(p, gid.x);
    }
    for (uint t = gtid; t < numTris; t += 32) {
        uint q = t / 2, x = q % 7, y = q / 7, v = y * 8 + x;
        tris[t] = (t & 1) ? uint3(v + 1, v + 9, v + 8) : uint3(v, v + 1, v + 8);
    }
}
