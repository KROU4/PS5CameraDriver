// Fast guided filter (He et al.) at working resolution. The final (a, b) coefficients are
// upsampled and applied with the full-resolution main luma, giving an edge-aligned disparity
// d(x) = a(x) * I(x) + b(x) anywhere in the frame.
//   ENTRY_PREP:  (I, p, I*I, I*p)
//   ENTRY_BOX:   separable box filter, horizontal when pathDir == 0, vertical otherwise
//   ENTRY_COEF:  (a, b) from the box means
#include "common.hlsli"

static const int kRadius = 4;

#if defined(ENTRY_PREP)
Texture2D<float> WorkMain : register(t0);
Texture2D<float> Disp : register(t1);
FORMAT("rgba32f") RWTexture2D<float4> Out : register(u0);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y) return;
    float I = WorkMain[id.xy];
    float p = Disp[id.xy];
    Out[id.xy] = float4(I, p, I * I, I * p);
}
#elif defined(ENTRY_BOX)
Texture2D<float4> In : register(t0);
FORMAT("rgba32f") RWTexture2D<float4> Out : register(u0);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y) return;
    int2 step = pathDir == 0 ? int2(1, 0) : int2(0, 1);
    float4 sum = 0;
    [unroll] for (int k = -kRadius; k <= kRadius; ++k)
        sum += In[clamp(int2(id.xy) + step * k, int2(0, 0), int2(workSize) - 1)];
    Out[id.xy] = sum / (2 * kRadius + 1);
}
#elif defined(ENTRY_COEF)
Texture2D<float4> Means : register(t0);
FORMAT("rgba32f") RWTexture2D<float4> Out : register(u0);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y) return;
    float4 m = Means[id.xy];
    float varI = max(m.z - m.x * m.x, 0);
    float covIp = m.w - m.x * m.y;
    float a = covIp / (varI + guidedEps);
    float b = m.y - a * m.x;
    Out[id.xy] = float4(a, b, 0, 0);
}
#endif
