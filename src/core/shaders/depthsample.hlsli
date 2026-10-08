// Edge-aware disparity lookup and circle-of-confusion, shared by the passes that read the depth
// (histogram, bokeh, composite, depthout, meter). One that uses the subject's share (subject.hlsl)
// defines SUBJECT_REGISTER, the texture register it binds the share to, before including this file.
Texture2D<float4> MainYuv : register(t0);
Texture2D<float4> Coef : register(t1);  // box-filtered guided coefficients (a, b)
#ifdef SUBJECT_REGISTER
Texture2D<float> SubjectShare : register(SUBJECT_REGISTER);
#endif

// How much farther the sharp zone reaches in front of the focus plane than behind it on the
// subject's own surface: as far as the share reaches (subject.hlsl, 1 / kNearer), the neck,
// shoulders, arms and hands in front of the face.
static const float kNearWiden = 4.0;

float2 EyeUvFromOutput(float2 outPx)
{
    float2 eye = crop.xy + outPx / float2(outSize) * crop.zw;
    return eye / float2(eyeSize);
}

float2 WorkUv(float2 eyeUv)
{
    return depthMirror != 0 ? float2(1.0 - eyeUv.x, eyeUv.y) : eyeUv;  // see downscale.hlsl
}

float DisparityAt(float2 eyeUv)
{
    float I = MainYuv.SampleLevel(LinearClamp, eyeUv, 0).x;
    float2 ab = Coef.SampleLevel(LinearClamp, WorkUv(eyeUv), 0).xy;
    return max(ab.x * I + ab.y, 0);
}

#ifdef SUBJECT_REGISTER
// 1 on the subject's own surface, 0 off it (0 everywhere without the silhouette).
float SubjectAt(float2 eyeUv)
{
    return subjectRange > 0 ? SubjectShare.SampleLevel(LinearClamp, WorkUv(eyeUv), 0) : 0;
}
#endif

// Blur radius in output pixels of disparity d with the given subject share: the sharp zone is
// focusRange wide, on the subject up to subjectRange more behind the focus plane and kNearWiden
// times that in front of it.
float CircleOfConfusion(float d, float subject)
{
    float diff = d - focusDisp;
    float excess = abs(diff) - focusRange - subject * subjectRange * (diff > 0 ? kNearWiden : 1.0);
    if (excess <= 0)
        return 0;
    float c = excess * blurScale * (diff > 0 ? fgScale : 1.0);
    return min(c, maxCoC);
}
