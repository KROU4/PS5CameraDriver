// Left-right consistency check followed by background hole filling, one thread per row:
// occlusions and rejected matches take the farther (smaller) of the nearest valid neighbours,
// which is what is normally hidden behind a foreground edge. Then temporal smoothing.
#include "common.hlsli"

Texture2D<float> DispMain : register(t0);
Texture2D<float> DispSecond : register(t1);
Texture2D<float> DispPrev : register(t2);
RWTexture2D<float> DispOut : register(u0);
RWTexture2D<float> LeftValid : register(u1);  // scratch: nearest valid value to the left, -1 if none

bool Consistent(int x, int y, out float d)
{
    d = DispMain[int2(x, y)];
    if (d < 0)
        return false;
    int xr = x - (int)round(d);
    if (xr < 0)
        return false;
    return abs(d - DispSecond[int2(xr, y)]) <= 1.5;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int y = id.x;
    if (y >= (int)workSize.y)
        return;
    int w = workSize.x;

    float last = -1;
    for (int x = 0; x < w; ++x)
    {
        float d;
        bool ok = Consistent(x, y, d);
        DispOut[int2(x, y)] = ok ? d : -1;
        LeftValid[int2(x, y)] = last;
        if (ok)
            last = d;
    }
    float right = -1;
    for (int x2 = w - 1; x2 >= 0; --x2)
    {
        float d = DispOut[int2(x2, y)];
        if (d >= 0)
        {
            right = d;
        }
        else
        {
            float left = LeftValid[int2(x2, y)];
            if (left >= 0 && right >= 0) d = min(left, right);
            else if (left >= 0) d = left;
            else if (right >= 0) d = right;
            else d = 0;
        }
        float prev = DispPrev[int2(x2, y)];
        // Blend with history only where the scene is static, so motion is not smeared.
        if (prev >= 0 && abs(prev - d) < 2.0)
            d = lerp(prev, d, temporalAlpha);
        DispOut[int2(x2, y)] = d;
    }
}
