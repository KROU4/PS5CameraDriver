// The subject's silhouette from depth, at working resolution: the cost of reaching each pixel from
// the in-focus plane (the face, within focusRange) along the picture. A step costs how much farther
// from the camera it goes (a quarter of that when it comes nearer), and a step back steeper than the
// subject's own surface has (a head's outline in front of the wall behind it) costs kJump times its
// excess on top. So the ears, hair and headphones, a head's depth behind the face, and the neck,
// shoulders and arms in front of it cost little: the subject is what the face reaches over its own
// surface. The background, however near, lies behind an outline. Share = 1 while the cost stays
// within subjectRange (the depth of a head), fading to 0 by 1.25 subjectRange; the blur passes widen
// the sharp zone by Share (depthsample.hlsli): subjectRange behind the focus plane, and in front of
// it as far as the share reaches (subjectRange / kNearer), while the full-resolution disparity still
// decides where an edge lies. focusRange and subjectRange come scaled to the subject's distance
// (pipeline.cpp); kSlope does not, which makes an outline a little easier to cross up close.
//   ENTRY_SEED:   disparity d = a I + b of the guided filter at working resolution; cost 0 within
//                 focusRange of the focus plane, unreached elsewhere
//   ENTRY_SWEEP:  one scan direction (pathDir: 0 right, 1 left, 2 down, 3 up), one thread per line:
//                 cost = min(cost, cost of the previous pixel + step cost). Two rounds of the four
//                 directions reach around a head and shoulders.
//   ENTRY_SHARE:  Share from the cost
#include "common.hlsli"

static const float kSlope = 0.25;   // steepest change per work pixel the subject's own surface has
static const float kJump = 4.0;
static const float kNearer = 0.25;  // cost per disparity unit of coming nearer; depthsample.hlsli kNearWiden
static const float kUnreached = 1e4;

#if defined(ENTRY_SEED)
Texture2D<float> WorkMain : register(t0);
Texture2D<float4> Coef : register(t1);  // box-filtered guided coefficients (a, b)
FORMAT("r32f") RWTexture2D<float> Disp : register(u0);
FORMAT("r32f") RWTexture2D<float> Cost : register(u1);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y)
        return;
    float2 ab = Coef[id.xy].xy;
    float d = max(ab.x * WorkMain[id.xy] + ab.y, 0);
    Disp[id.xy] = d;
    Cost[id.xy] = abs(d - focusDisp) <= focusRange ? 0 : kUnreached;
}
#elif defined(ENTRY_SWEEP)
// Reads the cost from one image and writes it to the other (the pipeline alternates them), so the
// reads of a line do not wait for its writes.
Texture2D<float> Disp : register(t0);
Texture2D<float> CostIn : register(t1);
FORMAT("r32f") RWTexture2D<float> CostOut : register(u0);
static const int2 kDirs[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 dir = kDirs[pathDir];
    uint lines = dir.y == 0 ? workSize.y : workSize.x;
    if (id.x >= lines)
        return;
    int steps = dir.y == 0 ? (int)workSize.x : (int)workSize.y;
    int2 p = dir.y == 0 ? int2(dir.x > 0 ? 0 : steps - 1, id.x) : int2(id.x, dir.y > 0 ? 0 : steps - 1);
    float prevD = Disp[p];
    float prevCost = CostIn[p];
    CostOut[p] = prevCost;
    for (int i = 1; i < steps; ++i)
    {
        p += dir;
        float d = Disp[p];
        float change = d - prevD;  // > 0: nearer
        float step = change > 0 ? kNearer * change : -change + kJump * max(-change - kSlope, 0);
        float cost = min(CostIn[p], prevCost + step);
        CostOut[p] = cost;
        prevD = d;
        prevCost = cost;
    }
}
#elif defined(ENTRY_SHARE)
// Smoothed over time like the disparity (temporalAlpha of the new frame), so that a chair just
// behind the head, whose cost hovers around subjectRange, does not flick between sharp and blurred;
// pathDir 0: no previous share to blend with.
Texture2D<float> Cost : register(t0);
Texture2D<float> PrevShare : register(t1);
FORMAT("r8") RWTexture2D<unorm float> Share : register(u0);  // R8: linear filtering on any GPU
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y)
        return;
    float share = 1.0 - smoothstep(subjectRange, 1.25 * subjectRange, Cost[id.xy]);
    if (pathDir != 0)
    {
        // At least one step of the 8-bit image towards the new value, or rounding would hold the
        // share a few steps short of it for good (strong smoothing: a sharp zone left behind).
        float prev = PrevShare[id.xy];
        float step = (share - prev) * temporalAlpha;
        share = prev + sign(share - prev) * min(abs(share - prev), max(abs(step), 1.0 / 255.0));
    }
    Share[id.xy] = share;
}
#endif
