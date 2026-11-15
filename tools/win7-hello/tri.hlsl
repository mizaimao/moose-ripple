// The triangle d3d10test.c draws: three vertices from a vertex buffer, red. Compiled to DXBC
// by hlsl2dxbc.c. MIT licence.
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; };

VSOut vs(float4 pos : POSITION)
{
    VSOut o;
    o.pos = pos;
    o.col = float4(1.0, 0.0, 0.0, 1.0);
    return o;
}

float4 ps(VSOut i) : SV_Target
{
    return i.col;
}
