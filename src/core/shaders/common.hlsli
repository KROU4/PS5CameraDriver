// Shared constants for every pass of the stereo bokeh pipeline. Must match core::GpuConstants.
cbuffer Constants : register(b0)
{
    uint2  eyeSize;       // full-resolution size of the main sensor image
    uint2  workSize;      // depth working resolution
    uint2  outSize;       // output (NV12) resolution
    uint   numDisp;       // disparity levels (== threads per aggregation group)
    uint   pathDir;       // aggregation direction index for CS_Aggregate
    float4 rectRow0;      // second-sensor affine, maps main pixel (x,y,1) -> second-sensor x, in main pixels
    float4 rectRow1;      //   ... -> second-sensor y
    float4 crop;          // eye-space rectangle (x, y, w, h) shown in the output
    float  focusDisp;     // in-focus disparity (working-res pixels)
    float  blurScale;     // output-pixel CoC per unit of disparity difference
    float  maxCoC;        // CoC clamp in output pixels
    float  fgScale;       // CoC multiplier for things nearer than the focus plane
    float  focusRange;    // disparity half-width of the sharp zone
    float  temporalAlpha; // weight of the new disparity in the temporal filter
    float  p1;            // SGM small-step penalty
    float  p2;            // SGM large-step penalty
    uint   mode;          // 0 bokeh, 1 main sensor, 2 second sensor, 3 depth view, 4 side by side,
                          // bench only: 5/6/7 raw / checked / hole-filled disparity, 8 disparity in grey
    float  guidedEps;     // guided filter regulariser
    uint   secondOffsetTexels; // texel x where the second sensor starts in the packed frame
    uint   mainOffsetTexels;   // texel x where the main sensor starts
    float  highlightGain; // bokeh highlight emphasis
    uint   outFormat;     // 0 NV12 planes, 1 packed YUY2
    float  lumaGain;      // digital exposure compensation applied to the output (1 = off)
    float  pad;
    uint2  secondSize;    // second sensor image as stored in the packed frame (half size on e9)
    uint   secondFolded;  // 1: each second-sensor row spans two frame lines, starting at line 1
    uint   depthMirror;   // 1: work images are mirrored, so the second sensor's match lies at x - d
                          //    although the main sensor is the left stream (see downscale.hlsl)
    float  noiseLevel;    // typical frame-to-frame change of a 3x3 luma mean in a still scene
    float  denoiseKeep;   // share of the new frame kept where nothing moves (1 = no temporal denoise)
    uint   denoiseHistory; // 0: no previous frame to blend with
    float  denoiseSpatial; // share of the spatially smoothed luma where something moves (0..1)
};

SamplerState LinearClamp : register(s0);

static const uint kCensusW = 9;
static const uint kCensusH = 7;
static const uint kInvalidDisp = 0xFFFF;

// Disparity levels; must equal kNumDisp in pipeline.cpp (one thread per level in aggregate.hlsl).
#define MAX_DISP 64
