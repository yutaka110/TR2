Texture2D<float> gLuma : register(t6);
Texture2D<float2> gChroma : register(t7);
RWTexture2D<float4> gOutput : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width = 0;
    uint height = 0;
    gOutput.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }

    float y = gLuma.Load(int3(tid.xy, 0));
    float2 uv = gChroma.Load(int3(tid.xy / 2, 0)) - float2(0.5f, 0.5f);

    float3 rgb;
    rgb.r = y + 1.402f * uv.y;
    rgb.g = y - 0.344136f * uv.x - 0.714136f * uv.y;
    rgb.b = y + 1.772f * uv.x;

    gOutput[tid.xy] = float4(saturate(rgb), 1.0f);
}
