// Rectification search: sums the best raw census cost of every textured pixel. A lower total
// means the rows of the two (rectified) images line up better.
#include "common.hlsli"

Texture2D<uint2> CensusMain : register(t0);
Texture2D<uint2> CensusSecond : register(t1);
RWByteAddressBuffer Total : register(u0);  // [0] cost sum, [1] pixel count

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < MAX_DISP || id.x >= workSize.x || id.y < 4 || id.y + 4 >= workSize.y)
        return;
    uint2 a = CensusMain[id.xy];
    uint ones = countbits(a.x) + countbits(a.y);
    if (ones < 8 || ones > 54)  // flat neighbourhoods carry no alignment information
        return;
    uint best = 64;
    for (uint d = 0; d < MAX_DISP; ++d)
    {
        uint2 b = CensusSecond[uint2(id.x - d, id.y)];
        best = min(best, countbits(a.x ^ b.x) + countbits(a.y ^ b.y));
    }
    Total.InterlockedAdd(0, best);
    Total.InterlockedAdd(4, 1);
}
