// Winner-takes-all on the aggregated volume: sub-pixel disparity for the main view with a
// uniqueness test, plus the integer disparity of the second view for the left-right check.
#include "common.hlsli"

ByteAddressBuffer Sum : register(t0);
Texture2D<float> WorkMain : register(t1);
Texture2D<uint2> CensusMain : register(t2);
Texture2D<uint2> CensusSecond : register(t3);
FORMAT("r32f") RWTexture2D<float> DispMain : register(u0);   // -1 = invalid
FORMAT("r32f") RWTexture2D<float> DispSecond : register(u1);

// The best cost must beat every other disparity by this share, or the match is ambiguous.
static const uint kUniquenessPercent = 92;
// A flat neighbourhood (luma standard deviation below this, sensor noise level) matches anywhere:
// SGM then just carries a neighbour's disparity along its paths, which is how a subject's depth
// spreads over a plain wall. Such pixels are left to the hole filling instead.
static const float kMinTexture = 0.012;
// Census bits that differ at the chosen disparity, averaged over 3x3 (of 62). A real match differs
// in a few bits; in dark noisy areas (a ceiling at high gain) the winner is the least bad of random
// patterns, which SGM turns into blobs that pass the other tests.
static const uint kMaxCensusCost = 20;

uint CensusCost(int2 p, int d)
{
    uint sum = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = clamp(p + int2(dx, dy), int2(d, 0), int2(workSize) - 1);
            uint2 a = CensusMain[q];
            uint2 b = CensusSecond[int2(q.x - d, q.y)];
            sum += countbits(a.x ^ b.x) + countbits(a.y ^ b.y);
        }
    }
    return sum;
}

float LocalStd(int2 p)
{
    float s = 0, s2 = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            float v = WorkMain[clamp(p + int2(dx, dy), int2(0, 0), int2(workSize) - 1)];
            s += v;
            s2 += v * v;
        }
    }
    float mean = s / 25.0;
    return sqrt(max(s2 / 25.0 - mean * mean, 0));
}

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

    // Near the image border the census window (in either image) reaches past the image: those
    // matches are guesses. The cheap tests go first; HLSL && does not short-circuit, hence [branch].
    bool inside = id.y >= 3 && id.y + 3 < workSize.y && id.x >= bestD + 4 && id.x + 4 < workSize.x;
    bool confident = inside && (second == 0xFFFFFFFF || best * 100 < second * kUniquenessPercent);
    [branch] if (confident)
        confident = LocalStd(int2(id.xy)) >= kMinTexture;
    [branch] if (confident)
        confident = CensusCost(int2(id.xy), bestD) <= kMaxCensusCost * 9;
    float disp = -1;
    if (confident)
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
