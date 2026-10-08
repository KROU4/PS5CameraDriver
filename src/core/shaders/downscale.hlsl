// Builds working-resolution luma for both sensors. The second sensor is resampled through the
// rectification affine so that matching rows line up with the main sensor; it is sampled in
// normalised coordinates, so it may be stored at another size than the main one.
// The matching passes expect the second sensor's view of a point at x - d. That holds when the
// main sensor is the right stream; with the left stream as main (depthMirror) both work images are
// stored mirrored, and DisparityAt mirrors back.
#include "common.hlsli"

Texture2D<float4> MainYuv : register(t0);
Texture2D<float4> SecondYuv : register(t1);
FORMAT("r32f") RWTexture2D<float> WorkMain : register(u0);
FORMAT("r32f") RWTexture2D<float> WorkSecond : register(u1);

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= workSize.x || id.y >= workSize.y)
        return;
    float2 scale = float2(eyeSize) / float2(workSize);
    float2 invEye = 1.0 / float2(eyeSize);
    float sumM = 0, sumS = 0;
    // 4x4 bilinear taps cover the footprint of one working pixel for scale factors up to ~4.
    [unroll] for (uint j = 0; j < 4; ++j)
    {
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            float2 e = (float2(id.xy) + (float2(i, j) + 0.5) / 4.0) * scale;
            if (depthMirror != 0)
                e.x = float(eyeSize.x) - e.x;
            sumM += MainYuv.SampleLevel(LinearClamp, e * invEye, 0).x;
            float2 r = float2(dot(rectRow0.xyz, float3(e, 1)), dot(rectRow1.xyz, float3(e, 1)));
            sumS += SecondYuv.SampleLevel(LinearClamp, r * invEye, 0).x;
        }
    }
    WorkMain[id.xy] = sumM / 16.0;
    WorkSecond[id.xy] = sumS / 16.0;
}
