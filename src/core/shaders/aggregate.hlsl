// Semi-global matching: aggregates census matching costs along one scan direction and adds the
// result into the shared sum volume. One thread group walks one scan line; thread d owns
// disparity d. Costs are computed on the fly from the census images (no cost volume).
// Directions run as separate dispatches, so every sum word is touched by exactly one thread
// per dispatch and plain load/store is race free.
#include "common.hlsli"

Texture2D<uint2> CensusMain : register(t0);
Texture2D<uint2> CensusSecond : register(t1);
Texture2D<float> WorkMain : register(t2);
Texture2D<uint2> PictureMotion : register(t3); // full-resolution 32x32 blocks, only with valid motion history
RWByteAddressBuffer Sum : register(u0); // uint16 per (pixel, disparity), 32 words per pixel

groupshared uint gL[2][MAX_DISP + 2];
groupshared uint gMin[3];

static const int2 kDirs[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
    int2(1, 1), int2(-1, -1), int2(-1, 1), int2(1, -1) };

uint MatchCost(int2 p, uint d)
{
    if (p.x < (int)d)
        return 40;  // no counterpart in the second image: neutral-ish cost
    uint2 a = CensusMain[p];
    uint2 b = CensusSecond[int2(p.x - (int)d, p.y)];
    return countbits(a.x ^ b.x) + countbits(a.y ^ b.y);
}

[numthreads(MAX_DISP, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    uint d = tid.x;
    int2 dir = kDirs[pathDir];
    int2 p;
    uint steps;
    if (dir.y == 0)
    {
        if (gid.x >= workSize.y) return;
        p = int2(dir.x > 0 ? 0 : (int)workSize.x - 1, gid.x);
        steps = workSize.x;
    }
    else if (dir.x == 0)
    {
        if (gid.x >= workSize.x) return;
        p = int2(gid.x, dir.y > 0 ? 0 : (int)workSize.y - 1);
        steps = workSize.y;
    }
    else
    {
        if (gid.x >= workSize.x + workSize.y - 1) return;
        // Top/bottom row plus the entering side excluding its corner. Every pixel belongs
        // to exactly one diagonal of this direction, so sum words still have a single writer.
        if (gid.x < workSize.x)
            p = int2(gid.x, dir.y > 0 ? 0 : (int)workSize.y - 1);
        else
            p = int2(dir.x > 0 ? 0 : (int)workSize.x - 1,
                dir.y > 0 ? (int)(gid.x - workSize.x + 1) : (int)(workSize.x + workSize.y - 2 - gid.x));
        steps = min(dir.x > 0 ? (int)workSize.x - p.x : p.x + 1,
                    dir.y > 0 ? (int)workSize.y - p.y : p.y + 1);
    }

    // Sentinels outside [0, MAX_DISP) so that d-1 / d+1 lookups need no branches.
    if (d == 0)
    {
        gL[0][0] = 0xFFFF; gL[1][0] = 0xFFFF;
        gL[0][MAX_DISP + 1] = 0xFFFF; gL[1][MAX_DISP + 1] = 0xFFFF;
        gMin[0] = 0; gMin[1] = 0xFFFFFFFF; gMin[2] = 0xFFFFFFFF;
    }
    gL[0][d + 1] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint P1 = (uint)p1;
    float prevI = WorkMain[p];
    for (uint s = 0; s < steps; ++s)
    {
        uint cur = s & 1, nxt = cur ^ 1;
        uint mRead = s % 3, mWrite = (s + 1) % 3, mReset = (s + 2) % 3;
        float I = WorkMain[p];
        // Penalise disparity jumps less across intensity edges (likely object boundaries).
        uint P2 = max((uint)(p2 / (1.0 + 24.0 * abs(I - prevI))), P1 + 1);
        prevI = I;

        uint c = MatchCost(p, d);
        uint L = c;
        if (s > 0)
        {
            uint prevMin = gMin[mRead];
            uint best = min(gL[cur][d + 1], min(gL[cur][d], gL[cur][d + 2]) + P1);
            best = min(best, prevMin + P2);
            L = c + best - prevMin;
        }
        gL[nxt][d + 1] = L;
        InterlockedMin(gMin[mWrite], L);
        if (d == 0)
            gMin[mReset] = 0xFFFFFFFF;
        GroupMemoryBarrierWithGroupSync();

        // Half of the threads fold the pair (2k, 2k+1) into one 32-bit sum word.
        if (d < MAX_DISP / 2)
        {
            uint pixel = (uint)p.y * workSize.x + (uint)p.x;
            uint addr = (pixel * (MAX_DISP / 2) + d) * 4;
            // Diagonal context supplements the original paths; half weight avoids giving
            // corner-connected background enough support to spread over a moving contour.
            uint low = gL[nxt][2 * d + 1], high = gL[nxt][2 * d + 2];
            if (pathDir >= 4) {
                low /= 2;
                high /= 2;
                [branch] if ((denoiseHistory & 3) == 2) {
                    float2 eye = (float2(p) + 0.5) * float2(eyeSize) / float2(workSize);
                    if (depthMirror != 0) eye.x = eyeSize.x - eye.x;
                    uint2 block = min(uint2(eye) / 32, (eyeSize - 1) / 32);
                    // Retain the original four-path decision where the picture moves. Extra
                    // diagonal context otherwise raises depth jitter at a moving fine contour.
                    if (any(asint(PictureMotion[block]) != 0)) { low = 0; high = 0; }
                }
            }
            uint packed = low | (high << 16);
            if (pathDir == 0)
                Sum.Store(addr, packed);
            else
                Sum.Store(addr, Sum.Load(addr) + packed);
        }
        p += dir;
    }
}
