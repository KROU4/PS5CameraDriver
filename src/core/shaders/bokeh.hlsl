// Depth-of-field gather at half output resolution (scatter-as-gather, after D. Gustafsson,
// "Bokeh depth of field in a single pass"). Samples nearer than the centre keep their own blur
// size; samples behind it are limited so a sharp subject never smears into the background.
#include "common.hlsli"
#define SUBJECT_REGISTER t2
#include "depthsample.hlsli"

FORMAT("rgba16f") RWTexture2D<float4> BokehHalf : register(u0);  // (Y, U, V, CoC in output px)

static const float kGoldenAngle = 2.39996323;
static const float kRadScale = 0.6;  // ring spacing in half-res pixels

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 halfSize = outSize / 2;
    if (id.x >= halfSize.x || id.y >= halfSize.y)
        return;
    float2 outPx = (float2(id.xy) + 0.5) * 2.0;
    float2 uv = EyeUvFromOutput(outPx);
    float3 centre = MainYuv.SampleLevel(LinearClamp, uv, 0).xyz;
    float centreD = DisparityAt(uv);
    float centreCoC = CircleOfConfusion(centreD, SubjectAt(uv)) * 0.5;  // half-res pixels
    if (centreCoC < 0.5)
    {
        BokehHalf[id.xy] = float4(centre, 0);
        return;
    }

    float maxR = maxCoC * 0.5;
    float3 colour = centre;
    float total = 1.0;
    float radius = kRadScale;
    float angle = 0;
    // Brighter samples weigh more, which turns small highlights into visible bokeh discs.
    float centreW = 1.0 + highlightGain * pow(saturate((centre.x - 0.65) / 0.35), 2);
    colour *= centreW;
    total = centreW;
    [loop] for (; radius < maxR; angle += kGoldenAngle)
    {
        float2 tapOut = outPx + float2(cos(angle), sin(angle)) * radius * 2.0;
        float2 tapUv = EyeUvFromOutput(tapOut);
        float3 tap = MainYuv.SampleLevel(LinearClamp, tapUv, 0).xyz;
        float tapD = DisparityAt(tapUv);
        float tapCoC = CircleOfConfusion(tapD, SubjectAt(tapUv)) * 0.5;
        if (tapD < centreD)  // tap lies behind the centre pixel
            tapCoC = min(tapCoC, centreCoC * 2.0);
        float m = smoothstep(radius - 0.5, radius + 0.5, tapCoC);
        float w = 1.0 + highlightGain * pow(saturate((tap.x - 0.65) / 0.35), 2);
        colour += lerp(colour / total, tap * w, m);
        total += lerp(1.0, w, m);
        radius += kRadScale / radius;
    }
    BokehHalf[id.xy] = float4(colour / total, centreCoC * 2.0);
}
