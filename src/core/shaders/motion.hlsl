// Motion of the picture between the denoised history and this frame, for the noise reduction
// (denoise.hlsl): one vector per 32x32 block of the main sensor image, in whole pixels, such that
// this frame at p shows what the history shows at p - v. In a dim room the noise is larger than
// the texture of a face or a wall, so the search runs against the history (already denoised) on
// 4x-downscaled images first, and a vector other than zero has to match clearly better than zero
// (noise alone finds an offset that matches a flat area a little better); then it is refined in
// whole pixels at full resolution, where again the coarse vector stays unless another is clearly
// better. Groups of 64 threads, the block and its search window in group shared memory.
//   ENTRY_DOWN:   4x4 means of the luma of this frame and of the history (eyeSize / 4)
//   ENTRY_SEARCH: one group per block, the offsets of +-kRange small pixels (+-32 px) shared out
//                 over the threads: mean absolute difference over the block plus kBorder small
//                 pixels around it
//   ENTRY_REFINE: one group per block, one thread per offset of +-2 px around the coarse vector:
//                 mean absolute difference over the block at full resolution
#include "common.hlsli"

static const uint kBlock = 32;     // pixels; pipeline.cpp kMotionBlock
static const int kRange = 8;       // small pixels each way
static const int kBorder = 2;      // small pixels around the block in the coarse match
static const float kAccept = 0.8;  // a moving vector must cost less than this share of zero's
static const float kRefineAccept = 0.95;
static const float kRefineAcceptStill = 0.85;  // ... when the coarse search found no motion
static const uint kThreads = 64;

// Lowest cost wins, ties to the lower index: the cost (0..1) in the high 23 bits, the index below.
uint Key(float cost, uint index)
{
    return (min(uint(cost * 8388607.0), 8388607u) << 9) | index;
}

#if defined(ENTRY_DOWN)
Texture2D<float4> Cur : register(t0);   // main sensor, this frame (Y, U, V, 1)
Texture2D<float4> Prev : register(t1);  // the denoised history
FORMAT("r32f") RWTexture2D<float> SmallCur : register(u0);
FORMAT("r32f") RWTexture2D<float> SmallPrev : register(u1);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= eyeSize.x / 4 || id.y >= eyeSize.y / 4)
        return;
    float c = 0, p = 0;
    [unroll] for (uint y = 0; y < 4; ++y)
    {
        [unroll] for (uint x = 0; x < 4; ++x)
        {
            uint2 q = id.xy * 4 + uint2(x, y);
            c += Cur[q].x;
            p += Prev[q].x;
        }
    }
    SmallCur[id.xy] = c / 16.0;
    SmallPrev[id.xy] = p / 16.0;
}
#elif defined(ENTRY_SEARCH)
Texture2D<float> SmallCur : register(t0);
Texture2D<float> SmallPrev : register(t1);
FORMAT("rg32ui") RWTexture2D<uint2> Coarse : register(u0);  // asuint of the vector in full pixels

static const int kSmallBlock = kBlock / 4;
static const int kWindow = kSmallBlock + 2 * kBorder;    // 12
static const int kPrevSize = kWindow + 2 * kRange;       // 28
static const int kSide = 2 * kRange + 1;                 // 17 offsets each way
groupshared float gCur[kWindow * kWindow];
groupshared float gPrev[kPrevSize * kPrevSize];
groupshared uint gBest;
groupshared float gZero;

[numthreads(kThreads, 1, 1)]
void main(uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    const int2 last = int2(eyeSize / 4) - 1;
    const int2 origin = int2(gid.xy) * kSmallBlock - kBorder;  // window corner, small pixels
    for (uint i = index; i < uint(kWindow * kWindow); i += kThreads)
        gCur[i] = SmallCur[clamp(origin + int2(i % kWindow, i / kWindow), int2(0, 0), last)];
    for (uint j = index; j < uint(kPrevSize * kPrevSize); j += kThreads)
        gPrev[j] = SmallPrev[clamp(origin - kRange + int2(j % kPrevSize, j / kPrevSize), int2(0, 0), last)];
    if (index == 0)
        gBest = 0xFFFFFFFF;
    GroupMemoryBarrierWithGroupSync();

    // Offset k is v: this frame's window against the history's window moved by -v.
    for (uint k = index; k < uint(kSide * kSide); k += kThreads)
    {
        const int2 v = int2(k % kSide, k / kSide) - kRange;
        float cost = 0;
        for (int y = 0; y < kWindow; ++y)
            for (int x = 0; x < kWindow; ++x)
                cost += abs(gCur[y * kWindow + x] - gPrev[(y + kRange - v.y) * kPrevSize + (x + kRange - v.x)]);
        cost /= kWindow * kWindow;
        if (v.x == 0 && v.y == 0)
            gZero = cost;
        InterlockedMin(gBest, Key(cost, k));
    }
    GroupMemoryBarrierWithGroupSync();

    if (index == 0)
    {
        const uint best = gBest & 511;
        const float bestCost = float(gBest >> 9) / 8388607.0;
        int2 bv = int2(best % kSide, best / kSide) - kRange;
        if (bestCost >= kAccept * gZero)
            bv = 0;
        Coarse[gid.xy] = asuint(bv * 4);
    }
}
#elif defined(ENTRY_REFINE)
Texture2D<float4> Cur : register(t0);
Texture2D<float4> Prev : register(t1);
Texture2D<uint2> Coarse : register(t2);
FORMAT("rg32ui") RWTexture2D<uint2> Motion : register(u0);

static const int kPrevWindow = kBlock + 4;  // the block moved by the coarse vector, +-2 px
groupshared float gCur[kBlock * kBlock];
groupshared float gPrev[kPrevWindow * kPrevWindow];
groupshared uint gBest;
groupshared float gBase;

[numthreads(kThreads, 1, 1)]
void main(uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    const int2 coarse = asint(Coarse[gid.xy]);
    const int2 last = int2(eyeSize) - 1;
    const int2 origin = int2(gid.xy) * kBlock;
    for (uint i = index; i < kBlock * kBlock; i += kThreads)
        gCur[i] = Cur[min(origin + int2(i % kBlock, i / kBlock), last)].x;
    const int2 prevOrigin = origin - coarse - 2;
    for (uint j = index; j < uint(kPrevWindow * kPrevWindow); j += kThreads)
        gPrev[j] = Prev[clamp(prevOrigin + int2(j % kPrevWindow, j / kPrevWindow), int2(0, 0), last)].x;
    if (index == 0)
        gBest = 0xFFFFFFFF;
    GroupMemoryBarrierWithGroupSync();

    // Thread k < 25 tries the coarse vector plus d: this frame at p against the history at p - v.
    if (index < 25)
    {
        const int2 d = int2(index % 5, index / 5) - 2;
        float cost = 0;
        for (int y = 0; y < int(kBlock); ++y)
            for (int x = 0; x < int(kBlock); ++x)
                cost += abs(gCur[y * kBlock + x] - gPrev[(y - d.y + 2) * kPrevWindow + (x - d.x + 2)]);
        cost /= kBlock * kBlock;
        if (d.x == 0 && d.y == 0)
            gBase = cost;
        InterlockedMin(gBest, Key(cost, index));
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0)
    {
        const uint best = gBest & 511;
        const float bestCost = float(gBest >> 9) / 8388607.0;
        int2 bv = coarse + int2(best % 5, best / 5) - 2;
        if (bestCost >= (coarse.x == 0 && coarse.y == 0 ? kRefineAcceptStill : kRefineAccept) * gBase)
            bv = coarse;
        Motion[gid.xy] = asuint(bv);
    }
}
#endif
