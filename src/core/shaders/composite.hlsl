// Final pass: blends the sharp main image with the half-resolution bokeh layer (or renders a
// diagnostic view) and writes NV12 planes. One thread per 2x2 output block.
#include "common.hlsli"
#include "depthsample.hlsli"

Texture2D<float4> BokehHalf : register(t2);
Texture2D<float4> SecondYuv : register(t3);
Texture2D<float> DispRaw : register(t4);     // debug views 5 and 6 (work resolution, -1 = invalid)
Texture2D<float> DispFilled : register(t5);
RWTexture2D<unorm float> OutY : register(u0);
RWTexture2D<unorm float2> OutUV : register(u1);
RWTexture2D<unorm float4> OutYuy2 : register(u2);  // outSize.x/2 x outSize.y texels: Y0 U Y1 V

float3 Turbo(float t)
{
    // Polynomial fit of the Turbo colormap (A. Mikhailov), returns RGB.
    const float4 kr4 = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const float4 kg4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const float4 kb4 = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const float2 kr2 = float2(-152.94239396, 59.28637943);
    const float2 kg2 = float2(4.27729857, 2.82956604);
    const float2 kb2 = float2(-89.90310912, 27.34824973);
    t = saturate(t);
    float4 v4 = float4(1.0, t, t * t, t * t * t);
    float2 v2 = v4.zw * v4.z;
    return saturate(float3(dot(v4, kr4) + dot(v2, kr2), dot(v4, kg4) + dot(v2, kg2), dot(v4, kb4) + dot(v2, kb2)));
}

float3 RgbToYuv(float3 c)
{
    // BT.601 full range, matching the camera's colour matrix.
    float y = dot(c, float3(0.299, 0.587, 0.114));
    return float3(y, (c.b - y) * 0.564 + 0.5, (c.r - y) * 0.713 + 0.5);
}

float3 Shade(float2 outPx)
{
    float2 uv = EyeUvFromOutput(outPx);
    float3 result;
    if (mode == 1)
    {
        result = MainYuv.SampleLevel(LinearClamp, uv, 0).xyz;
    }
    else if (mode == 2)
    {
        result = SecondYuv.SampleLevel(LinearClamp, uv, 0).xyz;
    }
    else if (mode == 3)
    {
        result = RgbToYuv(Turbo(DisparityAt(uv) / 64.0));
    }
    else if (mode == 5 || mode == 6)
    {
        // Nearest work pixel, black where invalid.
        float2 workUv = depthMirror != 0 ? float2(1.0 - uv.x, uv.y) : uv;
        int2 wp = min(int2(workUv * float2(workSize)), int2(workSize) - 1);
        float d = mode == 5 ? DispRaw[wp] : DispFilled[wp];
        result = d < 0 ? float3(0, 0.5, 0.5) : RgbToYuv(Turbo(d / 64.0));
    }
    else if (mode == 4)
    {
        // Both sensors squeezed side by side: main on the left, second on the right.
        float2 o = outPx;
        bool second = o.x >= outSize.x * 0.5;
        o.x = (o.x - (second ? outSize.x * 0.5 : 0)) * 2.0;
        float2 suv = EyeUvFromOutput(o);
        result = second ? SecondYuv.SampleLevel(LinearClamp, suv, 0).xyz : MainYuv.SampleLevel(LinearClamp, suv, 0).xyz;
    }
    else
    {
        float3 sharp = MainYuv.SampleLevel(LinearClamp, uv, 0).xyz;
        float coc = CircleOfConfusion(DisparityAt(uv));
        float3 blurred = BokehHalf.SampleLevel(LinearClamp, outPx / float2(outSize), 0).xyz;
        result = lerp(sharp, blurred, smoothstep(0.5, 2.5, coc));
    }
    // Digital exposure for dim rooms: linear gain with a soft shoulder so highlights do not clip
    // hard; chroma follows the luma ratio so colours keep their saturation.
    if (lumaGain > 1.01 && mode != 3)
    {
        float y = result.x * lumaGain;
        const float knee = 0.75;
        float boosted = y < knee ? y : knee + (1.0 - knee) * (1.0 - exp(-(y - knee) / (1.0 - knee)));
        float ratio = result.x > 0.002 ? boosted / result.x : lumaGain;
        result.x = boosted;
        result.yz = 0.5 + (result.yz - 0.5) * min(ratio, lumaGain);
    }
    // The sensor delivers full-range YUV; cameras are expected to output studio range (16-235).
    result = saturate(result);
    return float3(16.0 / 255.0 + result.x * (219.0 / 255.0), 0.5 + (result.yz - 0.5) * (224.0 / 255.0));
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 base = id.xy * 2;
    if (base.x >= outSize.x || base.y >= outSize.y)
        return;
    float2 uvSum = 0;
    [unroll] for (uint j = 0; j < 2; ++j)
    {
        float3 a = Shade(float2(base + uint2(0, j)) + 0.5);
        float3 b = Shade(float2(base + uint2(1, j)) + 0.5);
        float2 rowUv = (a.yz + b.yz) * 0.5;
        if (outFormat == 1)
        {
            OutYuy2[uint2(id.x, base.y + j)] = float4(a.x, rowUv.x, b.x, rowUv.y);
        }
        else
        {
            OutY[base + uint2(0, j)] = a.x;
            OutY[base + uint2(1, j)] = b.x;
        }
        uvSum += rowUv;
    }
    if (outFormat != 1)
        OutUV[id.xy] = uvSum * 0.5;
}
