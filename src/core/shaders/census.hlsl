// 9x7 census transform (62 bits) for both working images.
#include "common.hlsli"

Texture2D<float> WorkMain : register(t0);
Texture2D<float> WorkSecond : register(t1);
RWTexture2D<uint2> CensusMain : register(u0);
RWTexture2D<uint2> CensusSecond : register(u1);

uint2 Census(Texture2D<float> img, int2 p)
{
    float c = img[p];
    uint2 bits = 0;
    uint n = 0;
    [unroll] for (int dy = -3; dy <= 3; ++dy)
    {
        [unroll] for (int dx = -4; dx <= 4; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(workSize) - 1);
            uint bit = img[q] < c ? 1u : 0u;
            if (n < 32) bits.x |= bit << n;
            else bits.y |= bit << (n - 32);
            ++n;
        }
    }
    return bits;
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y)
        return;
    CensusMain[id.xy] = Census(WorkMain, int2(id.xy));
    CensusSecond[id.xy] = Census(WorkSecond, int2(id.xy));
}
