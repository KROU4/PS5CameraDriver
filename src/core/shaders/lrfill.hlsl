// Left-right consistency and speckle check, then filling of occlusions along each row (one thread
// per row): the background next to a foreground edge that only one camera sees is a gap as wide as
// the disparity step between its two ends, and takes the farther (smaller) end. Other gaps (plain
// surfaces with no usable texture, rejected matches) stay unknown here; holefill.hlsl fills them
// in 2D, so a stray value is never dragged along a row into a streak.
#include "common.hlsli"

Texture2D<float> DispMain : register(t0);
Texture2D<float> DispSecond : register(t1);
RWTexture2D<float> DispOut : register(u0);    // -1 = still unknown
RWTexture2D<float> LeftValid : register(u1);  // scratch: x of the nearest valid value to the left, -1 if none

// A real surface gives its neighbours similar disparities; a lone value is a speckle (a random
// match in a weak-texture area), whatever the uniqueness test said.
static const int kSpeckleSupport = 8;  // of the 24 neighbours in a 5x5 window, within 1 disparity
// Occlusions are as wide as the disparity step at an edge (a subject in front of a wall: ~20-30),
// give or take the few pixels the checks reject next to the edge.
static const int kMaxGap = 32;
static const int kGapSlack = 3;

bool Supported(int x, int y, float d)
{
    int support = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            int2 q = clamp(int2(x + dx, y + dy), int2(0, 0), int2(workSize) - 1);
            float n = DispMain[q];
            support += (n >= 0 && abs(n - d) <= 1.0) ? 1 : 0;
        }
    }
    return support - 1 >= kSpeckleSupport;  // the centre counts itself
}

bool Consistent(int x, int y, out float d)
{
    d = DispMain[int2(x, y)];
    if (d < 0)
        return false;
    int xr = x - (int)round(d);
    if (xr < 0)
        return false;
    [branch] if (abs(d - DispSecond[int2(xr, y)]) > 1.5)
        return false;
    return Supported(x, y, d);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int y = id.x;
    if (y >= (int)workSize.y)
        return;
    int w = workSize.x;

    int lastX = -1;
    for (int x = 0; x < w; ++x)
    {
        float d;
        bool ok = Consistent(x, y, d);
        DispOut[int2(x, y)] = ok ? d : -1;
        LeftValid[int2(x, y)] = lastX;
        if (ok)
            lastX = x;
    }
    int rightX = -1;
    float right = -1;
    for (int x2 = w - 1; x2 >= 0; --x2)
    {
        float d = DispOut[int2(x2, y)];
        if (d >= 0)
        {
            rightX = x2;
            right = d;
            continue;
        }
        int leftX = (int)LeftValid[int2(x2, y)];
        if (leftX < 0 || rightX < 0)
            continue;
        float left = DispOut[int2(leftX, y)];
        int gap = rightX - leftX - 1;
        if (gap <= kMaxGap && gap <= (int)abs(right - left) + kGapSlack)
            DispOut[int2(x2, y)] = min(left, right);
    }
}
