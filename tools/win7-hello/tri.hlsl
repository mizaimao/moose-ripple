// The triangle d3d10test.c draws: three vertices made from SV_VertexID (no vertex buffer),
// red. Compiled to DXBC by hlsl2dxbc.c (see build-tests.sh). MIT licence.
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; };

VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2(id == 2 ? 1.0 : -1.0, id == 1 ? 1.0 : -1.0);
    if (id == 1) p.x = 0.0;
    o.pos = float4(p, 0.0, 1.0);
    o.col = float4(1.0, 0.0, 0.0, 1.0);
    return o;
}

float4 ps(VSOut i) : SV_Target
{
    return i.col;
}
