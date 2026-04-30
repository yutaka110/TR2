Texture2D<float4> gLowResolutionBloom : register(t0);
Texture2D<float4> gHighResolutionBloom : register(t1);
Texture2D<float4> gUnused2 : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gBlend;
    float gSoftKnee;
    float gAux3;
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

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);
    float3 lowBloom = gLowResolutionBloom.Sample(gSampler, uv).rgb;
    float3 highBloom = gHighResolutionBloom.Sample(gSampler, uv).rgb;
    float lowLuminance = dot(lowBloom, float3(0.2126f, 0.7152f, 0.0722f));
    float highLuminance = dot(highBloom, float3(0.2126f, 0.7152f, 0.0722f));
    float softKnee = max(gSoftKnee, 0.001f);
    float kneeWeight = smoothstep(max(highLuminance - softKnee, 0.0f), highLuminance + softKnee, lowLuminance);
    float blend = saturate(gBlend) * kneeWeight;
    float3 color = highBloom + lowBloom * blend * gIntensity;
    return float4(color, 1.0f);
}
