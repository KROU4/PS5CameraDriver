// Centre-weighted disparity histogram used by the CPU autofocus (64 bins of 1 disparity each).
#include "common.hlsli"
#include "depthsample.hlsli"

RWByteAddressBuffer Histogram : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // Sample on a quarter-output grid; plenty for a histogram.
    uint2 grid = outSize / 4;
    if (id.x >= grid.x || id.y >= grid.y)
        return;
    float2 outPx = (float2(id.xy) + 0.5) * 4.0;
    float2 centred = outPx / float2(outSize) - 0.5;
    // Weight 4 in the central ellipse, fading to 0 at the borders: subjects sit in the middle.
    float r = length(centred * float2(1.6, 1.4));
    uint weight = (uint)round(4.0 * saturate(1.2 - r * 1.4));
    if (weight == 0)
        return;
    float d = DisparityAt(EyeUvFromOutput(outPx));
    uint bin = min((uint)round(d), 63u);
    Histogram.InterlockedAdd(bin * 4, weight);
}
