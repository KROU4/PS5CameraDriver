// 64-bin luma histogram of the main sensor (sparse grid, centre weighted) for auto brightness.
#include "common.hlsli"

Texture2D<float4> MainYuv : register(t0);
RWByteAddressBuffer LumaHist : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 grid = eyeSize / 8;
    if (id.x >= grid.x || id.y >= grid.y)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(grid);
    float2 c = uv - 0.5;
    uint weight = dot(c, c) < 0.09 ? 2 : 1;
    float y = MainYuv.SampleLevel(LinearClamp, uv, 0).x;
    LumaHist.InterlockedAdd(min((uint)(y * 64.0), 63u) * 4, weight);
}
