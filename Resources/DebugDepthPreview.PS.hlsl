Texture2D<float> gDepthTexture : register(t0);
Texture2D<float4> gUnusedSecondary : register(t1);
Texture2D<float4> gUnusedTertiary : register(t2);
SamplerState gSampler : register(s0);

cbuffer DebugDepthParams : register(b0)
{
    float gNearPlane;
    float gFarPlane;
    float gDepthPower;
    float gDepthInvert;
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

float LinearizeDepth(float deviceDepth, float nearPlane, float farPlane)
{
    return (nearPlane * farPlane) / max(farPlane - deviceDepth * (farPlane - nearPlane), 1.0e-5f);
}

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);
    float deviceDepth = saturate(gDepthTexture.Sample(gSampler, uv));
    float linearDepth = LinearizeDepth(deviceDepth, max(gNearPlane, 0.001f), max(gFarPlane, gNearPlane + 0.001f));
    float normalizedDepth = saturate((linearDepth - gNearPlane) / max(gFarPlane - gNearPlane, 0.001f));
    float preview = gDepthInvert > 0.5f ? (1.0f - normalizedDepth) : normalizedDepth;
    preview = pow(saturate(preview), max(gDepthPower, 0.1f));

    float3 nearColor = float3(0.08f, 0.75f, 1.0f);
    float3 farColor = float3(0.02f, 0.02f, 0.03f);
    float3 color = lerp(nearColor, farColor, normalizedDepth);
    color *= preview;
    return float4(color, 1.0f);
}
