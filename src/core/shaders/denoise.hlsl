// Motion-adaptive temporal noise reduction of the main sensor image, plus edge-aware spatial
// smoothing. Where a 3x3 luma mean barely changes between frames (a change of the order of the
// noise), the new frame is blended into the history, which averages the sensor noise of a dim
// room away over several frames; where it changes more, something moves and the new frame is
// taken without history, so motion does not smear, but smoothed over neighbours of similar
// brightness instead: the 5x5 around it and a ring at twice that spacing, since this camera's noise
// comes in blobs of a few pixels that a 5x5 window cannot average out. The typical change of a
// still scene is measured every frame (a histogram the CPU reads back), so the thresholds and the
// spatial smoothing follow the camera's gain by themselves. Chroma, which carries the ugliest
// low-light noise and whose detail the eye hardly sees, is always smoothed, over 17x17 (every
// fourth pixel) among neighbours of similar brightness and colour.
// Motion compensation (denoiseHistory 2): where the block a pixel lies in moved (motion.hlsl) and
// moved alike in the previous frame (motion goes on, while the noise of a dim room now and then
// fakes a vector for one frame), the history comes from where the block was, if at this pixel that
// matches better than the history in place; so a face that turns or a hand that moves keeps being
// averaged over frames instead of smearing or falling back to grainy spatial smoothing.
#include "common.hlsli"

Texture2D<float4> Cur : register(t0);   // main sensor, this frame (Y, U, V, 1)
Texture2D<float4> Prev : register(t1);  // the previous result
Texture2D<uint2> Motion : register(t2); // one vector per 32x32 block (asuint), denoiseHistory 2
Texture2D<uint2> MotionBefore : register(t3);  // the same of the previous frame
FORMAT("rgba8") RWTexture2D<unorm float4> Out : register(u0);
RWByteAddressBuffer NoiseHist : register(u1);  // 128 bins of the 3x3-mean change, 1/4096 each

static const float kChromaSigma = 0.05;  // luma difference at which a neighbour's chroma counts ~60%
// Chroma difference (from the 3x3 mean) at which it counts ~60%: keeps a colour edge of equal
// brightness (lips on skin, a logo on cloth) from bleeding over the wide window.
static const float kChromaColourSigma = 0.03;
// Luma range sigma of the spatial smoothing per unit of noiseLevel: on this camera a pixel's noise
// is about 2.3 noiseLevel (the ISP's noise is correlated between neighbours).
static const float kLumaSigma = 4.5;
// Signed changes add along a moving line but cancel along each row or column for random noise. A
// cheap 3x3 test opens the 7x7 test; above 2.8..4.8 noiseLevel that larger support is fine motion.
static const float kFineMotionStart = 2.8;
static const float kFineMotionEnd = 4.8;
static const float kSpatialDetailNoise = 0.5;
static const uint kMotionBlock = 32;
static const int kMotionAgree = 2;  // pixels a block's vector may differ from its previous one

// The history's 3x3 luma mean around p - v, and its pixel by pixel difference from this frame's
// 3x3 around p (summed absolute differences: fine texture that the means hide counts here).
float3 PrevMatch(int2 p, int2 v, int2 last)
{
    float mean = 0, sad = 0;
    float3 rowDiff = 0, columnDiff = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            float h = Prev[clamp(q - v, int2(0, 0), last)].x;
            float d = Cur[q].x - h;
            mean += h;
            sad += abs(d);
            rowDiff[y + 1] += d;
            columnDiff[x + 1] += d;
        }
    }
    const float3 one = 1.0;
    float coherent = max(dot(abs(rowDiff), one), dot(abs(columnDiff), one)) / 9.0;
    return float3(mean / 9.0, sad, coherent);
}

// Directional signed change over a larger support. This runs only where the 3x3 version already
// exceeds the noise threshold, so flat areas keep the original cost of the filter.
float FineChange(int2 p, int2 v, int2 last)
{
    float columns[7] = {0, 0, 0, 0, 0, 0, 0};
    float rows = 0;
    [unroll] for (int y = -3; y <= 3; ++y)
    {
        float row = 0;
        [unroll] for (int x = -3; x <= 3; ++x)
        {
            int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            float d = Cur[q].x - Prev[clamp(q - v, int2(0, 0), last)].x;
            row += d;
            columns[x + 3] += d;
        }
        rows += abs(row);
    }
    float cols = 0;
    [unroll] for (int i = 0; i < 7; ++i)
        cols += abs(columns[i]);
    return max(rows, cols) / 49.0;
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= eyeSize.x || id.y >= eyeSize.y)
        return;
    int2 p = int2(id.xy);
    const int2 last = int2(eyeSize) - 1;
    float4 c = Cur[p];

    float meanCur = 0, meanPrev = 0, sadPrev = 0;
    float3 rowDiff = 0, columnDiff = 0;
    float2 meanChroma = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            float4 n = Cur[q];
            meanCur += n.x;
            meanChroma += n.yz;
            if (denoiseHistory != 0)
            {
                float h = Prev[q].x;
                float d = n.x - h;
                meanPrev += h;
                sadPrev += abs(d);
                rowDiff[y + 1] += d;
                columnDiff[x + 1] += d;
            }
        }
    }
    meanCur /= 9.0;
    meanPrev /= 9.0;
    meanChroma /= 9.0;
    const float3 one = 1.0;
    float coarseFineChange = max(dot(abs(rowDiff), one), dot(abs(columnDiff), one)) / 9.0;
    const float t = max(noiseLevel, 1.0 / 4096.0);

    // Neighbours are weighed by their difference from the 3x3 mean rather than from the pixel
    // itself, so that a lone noisy pixel does not keep itself. On a thin line or a sharp step in a
    // clean picture every neighbour can be far from that mean and every weight underflow to 0: the
    // pixel itself, with a token weight, then stands (rather than 0/0, which turned such lines black).
    const float sigmaY = max(kLumaSigma * noiseLevel, 0.01);
    float luma = c.x * 1e-4, lumaWeight = 1e-4;
    [unroll] for (int dy = -4; dy <= 4; ++dy)
    {
        [unroll] for (int dx = -4; dx <= 4; ++dx)
        {
            // The 5x5, and of the 9x9 around it every second pixel of its border ring.
            const bool inner = abs(dx) <= 2 && abs(dy) <= 2;
            const bool ring = max(abs(dx), abs(dy)) == 4 && (dx % 2) == 0 && (dy % 2) == 0;
            if (!inner && !ring)
                continue;
            float n = Cur[clamp(p + int2(dx, dy), int2(0, 0), last)].x;
            float d = n - meanCur;
            float wy = exp(-0.5 * (d * d) / (sigmaY * sigmaY));
            luma += n * wy;
            lumaWeight += wy;
        }
    }
    float2 chroma = c.yz * 1e-4;
    float chromaWeight = 1e-4;
    [unroll] for (int cy = -2; cy <= 2; ++cy)
    {
        [unroll] for (int cx = -2; cx <= 2; ++cx)
        {
            float4 n = Cur[clamp(p + int2(cx, cy) * 4, int2(0, 0), last)];
            float d = n.x - meanCur;
            float2 dc = n.yz - meanChroma;
            float wc = exp(-0.5 * (d * d) / (kChromaSigma * kChromaSigma) -
                           0.5 * dot(dc, dc) / (kChromaColourSigma * kChromaColourSigma));
            chroma += n.yz * wc;
            chromaWeight += wc;
        }
    }
    float spatialLuma = luma / lumaWeight;
    const float spatialDetail = c.x - spatialLuma;
    const float spatialDetailed = spatialLuma +
        sign(spatialDetail) * max(abs(spatialDetail) - kSpatialDetailNoise * t, 0.0);
    chroma /= chromaWeight;

    if (denoiseHistory == 0)
    {
        // With no history there is no evidence that residual detail is coherent rather than noise.
        Out[p] = float4(lerp(c.x, spatialLuma, denoiseSpatial), chroma, 0);
        return;
    }
    float change = abs(meanCur - meanPrev);
    // The noise is measured on the history in place, as without motion compensation.
    if ((id.x & 3) == 0 && (id.y & 3) == 0)
        NoiseHist.InterlockedAdd(min(uint(change * 4096.0), 127u) * 4, 1);

    int2 historyOffset = 0;
    int2 from = p;
    if ((denoiseHistory & 3) == 2)
    {
        const uint2 block = min(id.xy / kMotionBlock, (eyeSize - 1) / kMotionBlock);
        int2 v = asint(Motion[block]);
        int2 before = asint(MotionBefore[block]);
        if ((v.x != 0 || v.y != 0) && all(abs(v - before) <= kMotionAgree))
        {
            // Along the motion only where it matches this pixel's 3x3 better than the history in
            // place, pixel by pixel: still fine texture next to a moving hand stays in place.
            float3 moved = PrevMatch(p, v, last);
            if (moved.y < sadPrev)
            {
                change = abs(meanCur - moved.x);
                coarseFineChange = moved.z;
                historyOffset = v;
                from = clamp(p - v, int2(0, 0), last);
            }
        }
    }

    // A 3x3 mean hides a fine pattern which moves by one or two pixels. Confirm its directional
    // change over 7x7 before restoring detail; random noise which passes 3x3 cancels over 7x7.
    float4 prev = Prev[from];
    float rawFineMotion = 0;
    [branch] if ((denoiseHistory & 4) != 0 && coarseFineChange > kFineMotionStart * t)
        rawFineMotion = smoothstep(kFineMotionStart * t, kFineMotionEnd * t,
                                   FineChange(p, historyOffset, last));
    // Real camera noise also comes in spatially coherent blobs, but unlike moving hair or fabric
    // they do not persist at the same (motion-compensated) point. Keep this frame's raw confidence
    // in alpha and restore detail only after two consecutive frames agree.
    const float fineMotion = rawFineMotion * smoothstep(0.25, 0.5, prev.a);
    const float movingLuma = lerp(spatialLuma, spatialDetailed, fineMotion);
    const float moving = lerp(c.x, movingLuma, denoiseSpatial);

    // Still: keep denoiseKeep of the new frame; clearly moving (several times the still-scene
    // change): none of the history.
    float m = max(smoothstep(2.0 * t, 5.0 * t, change), fineMotion);
    float still = lerp(prev.x, c.x, denoiseKeep);
    float2 stillChroma = lerp(prev.yz, chroma, denoiseKeep);
    Out[p] = float4(lerp(still, moving, m), lerp(stillChroma, chroma, m), rawFineMotion);
}
