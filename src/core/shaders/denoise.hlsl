// Motion-adaptive temporal noise reduction of the main sensor image, plus edge-aware spatial
// smoothing. Where a 3x3 luma mean barely changes between frames (a change of the order of the
// noise), the new frame is blended into the history, which averages the sensor noise of a dim
// room away over several frames; where it changes more, something moves and the new frame is
// taken without history, so motion does not smear, but smoothed over 5x5 neighbours of similar
// brightness instead. The typical change of a still scene is measured every frame (a histogram
// the CPU reads back), so the thresholds and the spatial smoothing follow the camera's gain by
// themselves. Chroma, which carries the ugliest low-light noise, is always smoothed that way.
#include "common.hlsli"

Texture2D<float4> Cur : register(t0);   // main sensor, this frame (Y, U, V, 1)
Texture2D<float4> Prev : register(t1);  // the previous result
FORMAT("rgba8") RWTexture2D<unorm float4> Out : register(u0);
RWByteAddressBuffer NoiseHist : register(u1);  // 128 bins of the 3x3-mean change, 1/4096 each

static const float kChromaSigma = 0.05;  // luma difference at which a neighbour's chroma counts ~60%
// Luma range sigma of the spatial smoothing per unit of noiseLevel: on this camera a pixel's noise
// is about 2.3 noiseLevel (the ISP's noise is correlated between neighbours).
static const float kLumaSigma = 4.5;

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= eyeSize.x || id.y >= eyeSize.y)
        return;
    int2 p = int2(id.xy);
    const int2 last = int2(eyeSize) - 1;
    float4 c = Cur[p];

    float meanCur = 0, meanPrev = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            meanCur += Cur[q].x;
            if (denoiseHistory != 0)
                meanPrev += Prev[q].x;
        }
    }
    meanCur /= 9.0;
    meanPrev /= 9.0;

    // Neighbours are weighed by their difference from the 3x3 mean rather than from the pixel
    // itself, so that a lone noisy pixel does not keep itself. On a thin line or a sharp step in a
    // clean picture every neighbour can be far from that mean and every weight underflow to 0: the
    // pixel itself, with a token weight, then stands (rather than 0/0, which turned such lines black).
    const float sigmaY = max(kLumaSigma * noiseLevel, 0.01);
    float luma = c.x * 1e-4, lumaWeight = 1e-4;
    float2 chroma = c.yz * 1e-4;
    float chromaWeight = 1e-4;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            float4 n = Cur[clamp(p + int2(dx, dy), int2(0, 0), last)];
            float d = n.x - meanCur;
            float wy = exp(-0.5 * (d * d) / (sigmaY * sigmaY));
            float wc = exp(-0.5 * (d * d) / (kChromaSigma * kChromaSigma));
            luma += n.x * wy;
            lumaWeight += wy;
            chroma += n.yz * wc;
            chromaWeight += wc;
        }
    }
    // What a moving pixel shows: the new frame, smoothed as much as the setting asks for.
    const float moving = lerp(c.x, luma / lumaWeight, denoiseSpatial);
    chroma /= chromaWeight;

    if (denoiseHistory == 0)
    {
        Out[p] = float4(moving, chroma, 1);
        return;
    }
    float change = abs(meanCur - meanPrev);
    if ((id.x & 3) == 0 && (id.y & 3) == 0)
        NoiseHist.InterlockedAdd(min(uint(change * 4096.0), 127u) * 4, 1);

    // Still: keep denoiseKeep of the new frame; clearly moving (several times the still-scene
    // change): none of the history.
    float t = max(noiseLevel, 1.0 / 4096.0);
    float m = smoothstep(2.0 * t, 5.0 * t, change);
    float4 prev = Prev[p];
    float still = lerp(prev.x, c.x, denoiseKeep);
    float2 stillChroma = lerp(prev.yz, chroma, denoiseKeep);
    Out[p] = float4(lerp(still, moving, m), lerp(stillChroma, chroma, m), 1);
}
