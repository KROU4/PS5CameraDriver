// Rectification search: sums the best raw census cost over the disparity range of the pixels with
// real texture (5x5 luma spread at least kMinSpread, well above sensor noise). A lower mean means
// the rows of the two (rectified) images line up better. A plain or noisy area matches equally
// badly at any alignment: with it counted, the curve over the vertical offset was flat on real
// frames (15.5 of 62 bits at every offset) and the calibration never succeeded. Decisive pixels
// (best match kMargin bits ahead of every other disparity) are counted for the calibration trace.
#include "common.hlsli"

Texture2D<uint2> CensusMain : register(t0);
Texture2D<uint2> CensusSecond : register(t1);
Texture2D<float> WorkMain : register(t2);
RWByteAddressBuffer Total : register(u0);  // [0] cost sum, [1] pixel count, [2] decisive pixels

static const float kMinSpread = 0.03;
static const uint kMargin = 6;

float Spread(int2 p)
{
    float s = 0, s2 = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            float v = WorkMain[p + int2(dx, dy)];
            s += v;
            s2 += v * v;
        }
    }
    float mean = s / 25.0;
    return sqrt(max(s2 / 25.0 - mean * mean, 0));
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < MAX_DISP || id.x + 4 >= workSize.x || id.y < 4 || id.y + 4 >= workSize.y)
        return;
    uint2 a = CensusMain[id.xy];
    uint ones = countbits(a.x) + countbits(a.y);
    if (ones < 8 || ones > 54)  // flat neighbourhoods carry no alignment information
        return;
    if (Spread(int2(id.xy)) < kMinSpread)
        return;
    uint best = 64, bestD = 0;
    for (uint d = 0; d < MAX_DISP; ++d)
    {
        uint2 b = CensusSecond[uint2(id.x - d, id.y)];
        uint cost = countbits(a.x ^ b.x) + countbits(a.y ^ b.y);
        if (cost < best) { best = cost; bestD = d; }
    }
    // The runner-up away from the winner, read again rather than kept in an array (only the trace
    // uses it).
    uint second = 64;
    for (uint d2 = 0; d2 < MAX_DISP; ++d2)
    {
        if (d2 + 1 < bestD || d2 > bestD + 1)
        {
            uint2 b = CensusSecond[uint2(id.x - d2, id.y)];
            second = min(second, countbits(a.x ^ b.x) + countbits(a.y ^ b.y));
        }
    }
    Total.InterlockedAdd(0, best);
    Total.InterlockedAdd(4, 1);
    if (second >= best + kMargin)
        Total.InterlockedAdd(8, 1);
}
