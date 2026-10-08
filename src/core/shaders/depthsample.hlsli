// Edge-aware disparity lookup and circle-of-confusion, shared by histogram, bokeh and composite.
Texture2D<float4> MainYuv : register(t0);
Texture2D<float4> Coef : register(t1);  // box-filtered guided coefficients (a, b)

float2 EyeUvFromOutput(float2 outPx)
{
    float2 eye = crop.xy + outPx / float2(outSize) * crop.zw;
    return eye / float2(eyeSize);
}

float DisparityAt(float2 eyeUv)
{
    float I = MainYuv.SampleLevel(LinearClamp, eyeUv, 0).x;
    float2 workUv = depthMirror != 0 ? float2(1.0 - eyeUv.x, eyeUv.y) : eyeUv;  // see downscale.hlsl
    float2 ab = Coef.SampleLevel(LinearClamp, workUv, 0).xy;
    return max(ab.x * I + ab.y, 0);
}

// Blur radius in output pixels.
float CircleOfConfusion(float d)
{
    float diff = d - focusDisp;
    float excess = abs(diff) - focusRange;
    if (excess <= 0)
        return 0;
    float c = excess * blurScale * (diff > 0 ? fgScale : 1.0);
    return min(c, maxCoC);
}
