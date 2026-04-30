Texture2D<float4> gInputTexture : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gThresholdMin;
    float gThresholdMax;
    float gSoftKnee;
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
    float4 color = gInputTexture.Sample(gSampler, saturate(input.uv));
    float luminance = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float softKnee = max(gSoftKnee, 0.001f);
    float lower = max(gThresholdMin - softKnee, 0.0f);
    float upper = max(gThresholdMax + softKnee, lower + 0.001f);
    float threshold = smoothstep(lower, upper, luminance);
    return float4(color.rgb * threshold * gIntensity, color.a);
}
