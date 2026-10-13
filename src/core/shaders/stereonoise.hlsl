// Work-resolution experiments only: the full-resolution colour image remains untouched.
#include "common.hlsli"

#if defined(ENTRY_NOISE)
Texture2D<float> Main : register(t0);
Texture2D<float> MainBefore : register(t1);
Texture2D<float> Second : register(t2);
Texture2D<float> SecondBefore : register(t3);
RWByteAddressBuffer Hist : register(u0); // two 512-bin signed difference histograms, 1/4096
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y || (id.x & 3) != 0 || (id.y & 3) != 0) return;
    int mainBin = clamp(256 + (int)round((Main[id.xy] - MainBefore[id.xy]) * 4096.0), 0, 511);
    int secondBin = clamp(256 + (int)round((Second[id.xy] - SecondBefore[id.xy]) * 4096.0), 0, 511);
    Hist.InterlockedAdd(mainBin * 4, 1);
    Hist.InterlockedAdd((512 + secondBin) * 4, 1);
}
#elif defined(ENTRY_BALANCE)
Texture2D<float> Main : register(t0);
Texture2D<float> Second : register(t1);
FORMAT("r32f") RWTexture2D<float> MatchedMain : register(u0);
FORMAT("r32f") RWTexture2D<float> MatchedSecond : register(u1);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y) return;
    float2 sum = 0;
    static const float weights[3] = {0.25, 0.5, 0.25};
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(workSize) - 1);
            sum += float2(Main[p], Second[p]) * weights[x + 1] * weights[y + 1];
        }
    MatchedMain[id.xy] = lerp(Main[id.xy], sum.x, stereoBalanceMain);
    MatchedSecond[id.xy] = lerp(Second[id.xy], sum.y, stereoBalanceSecond);
}
#endif
