// Light metering of the subject for the auto brightness (pipeline.cpp UpdateGain): over a grid of
// cells across the picture, how much of each cell the bokeh keeps sharp (the subject: the focus
// plane and the subject's own surface) and the luma there. One thread per cell, 8x8 samples each.
// Cell (x, y) gets three floats at (y * kMeterCellsX + x) * 12: the sharp weight, the
// weighted luma, and the plain luma, all summed over the samples.
#include "common.hlsli"
#define SUBJECT_REGISTER t2
#include "depthsample.hlsli"

RWByteAddressBuffer Meter : register(u0);

static const uint kMeterCellsX = 32;  // pipeline.cpp kMeterCellsX, kMeterCellsY
static const uint kMeterCellsY = 18;
static const uint kSamples = 8;       // per cell and axis

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kMeterCellsX || id.y >= kMeterCellsY)
        return;
    float3 sum = 0;
    [loop] for (uint sy = 0; sy < kSamples; ++sy)
    {
        [loop] for (uint sx = 0; sx < kSamples; ++sx)
        {
            float2 cell = (float2(id.xy) + (float2(sx, sy) + 0.5) / kSamples) / float2(kMeterCellsX, kMeterCellsY);
            float2 uv = EyeUvFromOutput(cell * float2(outSize));
            float y = MainYuv.SampleLevel(LinearClamp, uv, 0).x;
            float w = 1.0 - smoothstep(0.5, 2.5, CircleOfConfusion(DisparityAt(uv), SubjectAt(uv)));
            sum += float3(w, w * y, y);
        }
    }
    Meter.Store3((id.y * kMeterCellsX + id.x) * 12, asuint(sum));
}
