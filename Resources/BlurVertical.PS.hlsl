Texture2D<float4> gInputTexture : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gBlurRadius;
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

float GaussianWeight(float x, float sigma)
{
    float safeSigma = max(sigma, 0.0001f);
    return exp(-0.5f * (x * x) / (safeSigma * safeSigma));
}

float3 SampleBlur(float2 uv)
{
    uint width;
    uint height;
    gInputTexture.GetDimensions(width, height);
    float2 texel = 1.0f / max(float2(width, height), 1.0f.xx);
    const int maxRadius = min((int)round(max(gBlurRadius, 1.0f)), 8);
    const float sigma = max(gBlurRadius * 0.5f, 0.75f);
    float totalWeight = GaussianWeight(0.0f, sigma);
    float3 color = gInputTexture.Sample(gSampler, uv).rgb * totalWeight;
    [unroll]
    for (int i = 1; i <= 8; ++i)
    {
        if (i > maxRadius)
        {
            break;
        }
        float weight = GaussianWeight((float)i, sigma);
        float2 offset = float2(0.0f, texel.y * i);
        color += gInputTexture.Sample(gSampler, uv + offset).rgb * weight;
        color += gInputTexture.Sample(gSampler, uv - offset).rgb * weight;
        totalWeight += weight * 2.0f;
    }
    return color / max(totalWeight, 0.0001f);
}

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);
    return float4(SampleBlur(uv) * gIntensity, 1.0f);
}
