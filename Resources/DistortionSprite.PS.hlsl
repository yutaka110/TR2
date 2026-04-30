Texture2D gDistortionTex : register(t0);
Texture2D<float> gSceneDepth : register(t1);
SamplerState gSampler : register(s0);

cbuffer VfxDrawCB : register(b0)
{
    float gDepthFadeSoftness;
    float gDistortionDepthAttenuation;
    float gParticleEdgeSoftness;
    float gTrailTailFade;
};

struct PSIn
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD;
    float4 color : COLOR0;
};

float ComputeDepthAttenuation(float4 position)
{
    uint depthWidth = 0;
    uint depthHeight = 0;
    gSceneDepth.GetDimensions(depthWidth, depthHeight);
    int2 pixelCoord = int2(
        clamp(position.x, 0.0f, max(float(depthWidth) - 1.0f, 0.0f)),
        clamp(position.y, 0.0f, max(float(depthHeight) - 1.0f, 0.0f)));
    float sceneDepth = gSceneDepth.Load(int3(pixelCoord, 0));
    float softness = max(gDepthFadeSoftness, 0.0001f);
    float attenuation = smoothstep(0.0f, softness, sceneDepth - position.z);
    return pow(attenuation, max(gDistortionDepthAttenuation, 0.01f));
}

float4 main(PSIn input) : SV_TARGET
{
    float4 tex = gDistortionTex.Sample(gSampler, input.texcoord);
    float2 centered = input.texcoord - 0.5f;
    float radial = length(centered) * 2.0f;
    float ring = smoothstep(1.0f, 0.15f, radial) * smoothstep(0.0f, 0.45f, radial);
    float noise = tex.r * 2.0f - 1.0f;
    float depthAttenuation = ComputeDepthAttenuation(input.position);

    float2 dir = radial > 0.001f ? centered / max(radial * 0.5f, 0.001f) : float2(0.0f, 0.0f);
    float2 offset = dir * (0.5f + noise * 0.35f) * input.color.a * depthAttenuation;

    float alpha = ring * saturate(input.color.a + tex.a * 0.5f) * depthAttenuation;
    return float4(0.5f + offset * 0.5f, 0.0f, alpha);
}
