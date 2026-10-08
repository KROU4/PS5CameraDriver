// Splits the packed YUY2 frame into both sensor images as (Y, U, V, 1) unorm textures, each at its
// own size (the second sensor is half size on firmware e9). One thread per YUY2 texel (two pixels)
// per eye.
#include "common.hlsli"

Texture2D<uint4> Packed : register(t0);             // R8G8B8A8_UINT: Y0 U Y1 V
FORMAT("rgba8") RWTexture2D<unorm float4> MainYuv : register(u0);   // eyeSize
FORMAT("rgba8") RWTexture2D<unorm float4> SecondYuv : register(u1); // secondSize

void Write(RWTexture2D<unorm float4> dst, uint2 p0, uint4 t)
{
    float u = t.y / 255.0, v = t.w / 255.0;
    dst[p0] = float4(t.x / 255.0, u, v, 1);
    dst[p0 + uint2(1, 0)] = float4(t.z / 255.0, u, v, 1);
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 p0 = uint2(id.x * 2, id.y);
    if (id.x < eyeSize.x / 2 && id.y < eyeSize.y)
        Write(MainYuv, p0, Packed[uint2(mainOffsetTexels + id.x, id.y)]);
    uint secondTexels = secondSize.x / 2;
    if (id.x >= secondTexels || id.y >= secondSize.y)
        return;
    uint2 src = uint2(secondOffsetTexels + id.x, id.y);
    if (secondFolded != 0)
    {
        // Row y occupies frame lines 1 + 2y (first half of its texels) and 2 + 2y (second half).
        uint perLine = secondTexels / 2;
        uint part = id.x / perLine;
        src = uint2(secondOffsetTexels + id.x - part * perLine, 1 + 2 * id.y + part);
    }
    Write(SecondYuv, p0, Packed[src]);
}
