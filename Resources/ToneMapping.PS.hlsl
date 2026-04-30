Texture2D<float4> gInputTexture : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gToneExposure;
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

float3 ToneMap(float3 color)
{
    return color / (1.0f + color);
}

float4 main(PSInput input) : SV_TARGET
{
    float exposure = max(gToneExposure, 0.0001f);
    float3 color = gInputTexture.Sample(gSampler, saturate(input.uv)).rgb * exposure * max(gIntensity, 0.0001f);
    return float4(ToneMap(color), 1.0f);
}
