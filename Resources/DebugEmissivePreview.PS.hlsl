Texture2D<float4> gSceneColor : register(t0);
Texture2D<float4> gVfxAccumulation : register(t1);
Texture2D<float4> gPostColor : register(t2);
SamplerState gSampler : register(s0);

cbuffer DebugEmissiveParams : register(b0)
{
    float gEmissiveBoost;
    float gVfxWeight;
    float gPostDeltaWeight;
    float gHeatmapMix;
    float gAux4;
    float gAux5;
    float gAux6;
    float gAux7;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float3 Heatmap(float value)
{
    float x = saturate(value);
    float3 cold = float3(0.03f, 0.05f, 0.12f);
    float3 mid = float3(0.18f, 0.55f, 1.0f);
    float3 hot = float3(1.0f, 0.82f, 0.2f);
    float3 white = float3(1.0f, 1.0f, 1.0f);
    float3 color = lerp(cold, mid, saturate(x * 2.0f));
    color = lerp(color, hot, saturate(x * 1.4f - 0.3f));
    color = lerp(color, white, saturate(x * 1.1f - 0.75f));
    return color;
}

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);
    float3 scene = gSceneColor.Sample(gSampler, uv).rgb;
    float3 vfx = gVfxAccumulation.Sample(gSampler, uv).rgb;
    float3 post = gPostColor.Sample(gSampler, uv).rgb;

    float3 postDelta = max(post - scene, 0.0f.xxx);
    float3 emissive = vfx * gVfxWeight + postDelta * gPostDeltaWeight;
    emissive *= max(gEmissiveBoost, 0.0f);

    float luminance = dot(emissive, float3(0.2126f, 0.7152f, 0.0722f));
    float3 heatmap = Heatmap(luminance);
    float3 isolated = emissive;
    float3 color = lerp(isolated, heatmap, saturate(gHeatmapMix));
    return float4(saturate(color), 1.0f);
}
