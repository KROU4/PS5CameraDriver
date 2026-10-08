// Winner-takes-all on the aggregated volume: sub-pixel disparity for the main view with a
// uniqueness test, plus the integer disparity of the second view for the left-right check.
#include "common.hlsli"

ByteAddressBuffer Sum : register(t0);
RWTexture2D<float> DispMain : register(u0);   // -1 = invalid
RWTexture2D<float> DispSecond : register(u1);

uint SumAt(uint pixel, uint d)
{
    uint w = Sum.Load((pixel * (MAX_DISP / 2) + (d >> 1)) * 4);
    return (d & 1) ? (w >> 16) : (w & 0xFFFF);
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y)
        return;
    uint row = id.y * workSize.x;
    uint pixel = row + id.x;

    uint costs[MAX_DISP];
    uint best = 0xFFFFFFFF, bestD = 0;
    [unroll] for (uint k = 0; k < MAX_DISP / 2; ++k)
    {
        uint w = Sum.Load((pixel * (MAX_DISP / 2) + k) * 4);
        costs[2 * k] = w & 0xFFFF;
        costs[2 * k + 1] = w >> 16;
    }
    for (uint d = 0; d < MAX_DISP && d <= id.x; ++d)
    {
        if (costs[d] < best) { best = costs[d]; bestD = d; }
    }
    uint second = 0xFFFFFFFF;
    for (uint d2 = 0; d2 < MAX_DISP && d2 <= id.x; ++d2)
    {
        if (d2 + 1 < bestD || d2 > bestD + 1)
            second = min(second, costs[d2]);
    }

    float disp = -1;
    if (second == 0xFFFFFFFF || best * 100 < second * 97)
    {
        disp = bestD;
        if (bestD > 0 && bestD + 1 < MAX_DISP && bestD + 1 <= id.x)
        {
            float c0 = costs[bestD - 1], c1 = costs[bestD], c2 = costs[bestD + 1];
            float denom = c0 - 2 * c1 + c2;
            if (denom > 0)
                disp += clamp((c0 - c2) / (2 * denom), -0.5, 0.5);
        }
    }
    DispMain[id.xy] = disp;

    // Second view: pixel xr matches main pixel xr + d at disparity d.
    uint bestR = 0xFFFFFFFF, bestRD = 0;
    for (uint dr = 0; dr < MAX_DISP && id.x + dr < workSize.x; ++dr)
    {
        uint c = SumAt(row + id.x + dr, dr);
        if (c < bestR) { bestR = c; bestRD = dr; }
    }
    DispSecond[id.xy] = bestRD;
}
