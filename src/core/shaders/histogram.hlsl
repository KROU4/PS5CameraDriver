// Centre-weighted disparity histograms used by the CPU autofocus (64 bins of 1 disparity each): bins
// 0-63 of the points the stereo matching measured (DispFill: after the left-right check and the
// occlusion fill along rows, -1 = unknown), where the autofocus looks for the subject, since what the
// hole fill guessed spreads over plain areas and made peaks of its own, which the focus then chased;
// bins 64-127 of every point, from which it takes the focus value, in the same disparity the blur
// compares with (the filled depth of a face sits a little behind its measured points).
#include "common.hlsli"
#include "depthsample.hlsli"

Texture2D<float> DispFill : register(t2);
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
    float2 eyeUv = EyeUvFromOutput(outPx);
    float d = DisparityAt(eyeUv);
    uint bin = min((uint)round(d), MAX_DISP - 1u);
    Histogram.InterlockedAdd((MAX_DISP + bin) * 4, weight);
    int2 wp = min(int2(WorkUv(eyeUv) * float2(workSize)), int2(workSize) - 1);
    if (DispFill[wp] >= 0)
        Histogram.InterlockedAdd(bin * 4, weight);
}
