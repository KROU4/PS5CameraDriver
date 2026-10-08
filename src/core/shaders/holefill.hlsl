// Fills what lrfill.hlsl left unknown (plain surfaces with no usable texture, wide occlusions) from
// valid values in eight directions, taking their lower median: a gap next to a subject gets the
// background unless the subject surrounds it, and one stray value can no longer be dragged across
// a whole wall as a fill along rows does. Each ray prefers the nearest valid value of similar
// brightness (likely the same surface) to the nearest one, so a patch of wall framed by the arms
// takes the wall behind them rather than the arms. Then temporal smoothing.
#include "common.hlsli"

Texture2D<float> DispIn : register(t0);    // -1 = unknown
Texture2D<float> DispPrev : register(t1);  // the previous frame's result, -1 = none
Texture2D<float> WorkMain : register(t2);  // luma at work resolution
RWTexture2D<float> DispOut : register(u0);

// Sample distances along a direction: dense nearby, sparse far away (reaches 256 work pixels). A
// valid strip narrower than a far step can be skipped; the ray then takes the next value beyond it.
static const int kSteps = 16;
static const int kDist[kSteps] = {1, 2, 3, 4, 6, 8, 11, 16, 22, 32, 45, 64, 90, 128, 180, 256};
// Filled values change slowly (this share of the temporal weight) unless they drop by more than
// kFillDrop, which is a subject moving off a plain background.
static const float kFillRate = 0.25;
static const float kFillDrop = 6.0;
static const int2 kDirs[8] = {
    int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)};
// Same surface: luma within this of the hole's, looked for up to kDist[kSimilarSteps - 1] away
// (farther, a similar brightness is as likely another object).
static const float kSameSurface = 0.04;
static const int kSimilarSteps = 12;

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y)
        return;
    int2 p = int2(id.xy);
    float d = DispIn[p];
    float prev = DispPrev[p];
    bool measured = d >= 0;
    if (!measured)
    {
        float found[8];  // per direction, -1 = nothing within reach
        uint n = 0;
        const float luma = WorkMain[p];
        [unroll] for (uint k = 0; k < 8; ++k)
        {
            float nearest = -1;
            found[k] = -1;
            [loop] for (int s = 0; s < kSteps; ++s)
            {
                int2 q = p + kDirs[k] * kDist[s];
                if (any(q < 0) || any(q >= int2(workSize)))
                    break;
                float v = DispIn[q];
                if (v < 0)
                    continue;
                if (nearest < 0)
                    nearest = v;
                if (abs(WorkMain[q] - luma) <= kSameSurface)
                {
                    found[k] = v;
                    break;
                }
                if (s + 1 >= kSimilarSteps)
                    break;
            }
            if (found[k] < 0)
                found[k] = nearest;
            n += found[k] >= 0 ? 1 : 0;
        }
        // Nothing usable anywhere around (a dark or blank frame): keep the last value, else far.
        d = max(prev, 0.0);
        if (n > 0)
        {
            uint want = (n - 1) / 2;  // rank of the lower median
            [unroll] for (uint i = 0; i < 8; ++i)
            {
                uint rank = 0;
                [unroll] for (uint j = 0; j < 8; ++j)
                    rank += (found[j] >= 0 && (found[j] < found[i] || (found[j] == found[i] && j < i))) ? 1 : 0;
                if (found[i] >= 0 && rank == want)
                    d = found[i];
            }
        }
    }
    // Blend with history only where the scene is static, so motion is not smeared. A filled value
    // is only an estimate (in a dark plain area often from a few noise matches), and one that comes
    // nearer turns a patch of background sharp: it follows slowly. Only a large drop (the subject
    // moved away and uncovered the background) is taken at once, so no sharp ghost trails it; a
    // fast drop for small changes too would ratchet noisy fills on the subject towards the back.
    if (prev >= 0 && abs(prev - d) < 2.0)
        d = lerp(prev, d, temporalAlpha);
    else if (prev >= 0 && !measured && d > prev - kFillDrop)
        d = lerp(prev, d, max(temporalAlpha * kFillRate, 0.05));  // not seconds at the lowest temporal setting
    DispOut[p] = d;
}
