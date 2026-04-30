Texture2D<float4> gInputTexture : register(t0);
Texture2D<float4> gSceneColor : register(t1);
Texture2D<float4> gFallbackTexture : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gDistortionScale;
    float gAux1;
    float gAux2;
    float gAux3;
    float gAux4;
    float gAux5;
    float gAux6;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);
    float4 distortion = gInputTexture.Sample(gSampler, uv);
    float2 offset = (distortion.rg - 0.5f) * distortion.a * gDistortionScale * gIntensity;
    float3 scene = gSceneColor.Sample(gSampler, saturate(uv + offset)).rgb;
    float3 fallbackColor = gFallbackTexture.Sample(gSampler, uv).rgb;
    return float4(scene + fallbackColor * 0.15f, 1.0f);
}
