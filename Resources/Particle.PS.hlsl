Texture2D gParticleTex : register(t0);
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

float ComputeSoftParticleFade(float4 position)
{
    uint depthWidth = 0;
    uint depthHeight = 0;
    gSceneDepth.GetDimensions(depthWidth, depthHeight);
    int2 pixelCoord = int2(
        clamp(position.x, 0.0f, max(float(depthWidth) - 1.0f, 0.0f)),
        clamp(position.y, 0.0f, max(float(depthHeight) - 1.0f, 0.0f)));
    float sceneDepth = gSceneDepth.Load(int3(pixelCoord, 0));
    float softness = max(gDepthFadeSoftness, 0.0001f);
    return smoothstep(0.0f, softness, sceneDepth - position.z);
}

float ComputeEdgeFade(float2 uv)
{
    float centered = abs(uv.x - 0.5f) * 2.0f;
    float softness = saturate(gParticleEdgeSoftness);
    float inner = saturate(1.0f - centered);
    float exponent = lerp(4.0f, 0.75f, softness);
    return pow(inner, exponent);
}

float4 main(PSIn input) : SV_TARGET
{
    float4 tex = gParticleTex.Sample(gSampler, input.texcoord);
    float4 outc = tex * input.color;
    outc.a *= ComputeSoftParticleFade(input.position);
    outc.a *= ComputeEdgeFade(input.texcoord);
    outc.rgb *= outc.a;
    return outc;
}
