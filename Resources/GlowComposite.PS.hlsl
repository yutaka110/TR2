Texture2D<float4> gInputTexture : register(t0);
Texture2D<float4> gRawVfxTexture : register(t1);
Texture2D<float4> gBaseTexture : register(t2);
SamplerState gSampler : register(s0);

cbuffer PostProcessParams : register(b0)
{
    float gIntensity;
    float gGlowWeight;
    float gGlowTintR;
    float gGlowTintG;
    float gGlowTintB;
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
    float4 glowSource = gInputTexture.Sample(gSampler, uv);
    float4 rawVfx = gRawVfxTexture.Sample(gSampler, uv);
    float3 baseColor = gBaseTexture.Sample(gSampler, uv).rgb;
    float3 glowTint = float3(gGlowTintR, gGlowTintG, gGlowTintB);
    float3 rawContribution = rawVfx.rgb * max(rawVfx.a, 0.2f) * gIntensity;
    float3 glow = glowSource.rgb * glowTint * max(glowSource.a, 0.35f) * gGlowWeight * gIntensity;
    return float4(baseColor + rawContribution + glow, 1.0f);
}
