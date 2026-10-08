// The depth plane for the "PS5 Camera Depth" camera: one byte per output pixel, aligned with the
// picture (same crop, same edge-aware disparity as the bokeh). depthView 0: the disparity in studio
// range (near = bright; 16 at infinity, 235 at the nearest disparity); 1: the subject matte for
// removing the background, white for the subject in focus and everything in front of it, fading to
// black behind it as the bokeh's blur grows.
#include "common.hlsli"
#include "depthsample.hlsli"

RWTexture2D<unorm float> DepthOut : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outSize.x || id.y >= outSize.y)
        return;
    float2 uv = EyeUvFromOutput(float2(id.xy) + 0.5);
    float d = DisparityAt(uv);
    float v;
    if (depthView == 0)
    {
        v = saturate(d / float(MAX_DISP));
    }
    else
    {
        // Behind the focus plane the matte is what the bokeh keeps sharp (composite blends with
        // smoothstep(0.5, 2.5, CoC)); in front of it everything belongs to the subject.
        v = d >= focusDisp ? 1.0 : 1.0 - smoothstep(0.5, 2.5, CircleOfConfusion(d));
    }
    DepthOut[id.xy] = 16.0 / 255.0 + v * (219.0 / 255.0);
}
