Texture2D<float4> gInputTexture : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gAux1;
    float gAux2;
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
    uint width;
    uint height;
    gInputTexture.GetDimensions(width, height);
    float2 texel = 1.0f / max(float2(width, height), 1.0f.xx);
    float2 uv = saturate(input.uv);

    float3 color = gInputTexture.Sample(gSampler, uv).rgb * 0.25f;
    color += gInputTexture.Sample(gSampler, uv + float2(texel.x, texel.y)).rgb * 0.1875f;
    color += gInputTexture.Sample(gSampler, uv + float2(-texel.x, texel.y)).rgb * 0.1875f;
    color += gInputTexture.Sample(gSampler, uv + float2(texel.x, -texel.y)).rgb * 0.1875f;
    color += gInputTexture.Sample(gSampler, uv + float2(-texel.x, -texel.y)).rgb * 0.1875f;
    return float4(color * gIntensity, 1.0f);
}
